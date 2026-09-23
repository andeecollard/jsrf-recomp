/* The host DSOUND's output: dsh_mix pulled by the audio device, or by a
 * real-time thread when there is no device. See dsound_host.h. */
#include "dsound_host.h"

#include <stdio.h>

#if !defined(_WIN32)
#include <SDL.h>
#include <stdlib.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>

static SDL_AudioDeviceID g_dev;
static int g_started;

/* RECOMP_DSOUND_LIFT_WAV=<path>: every frame the lift mixes is also written
 * to a 48 kHz stereo WAV, so a silenced harness run can be checked by
 * script -- clipping, silence, dropouts -- without playing anything aloud.
 * The header is rewritten every second of audio, so a killed run still leaves
 * a readable file. */
static FILE *g_wav;
static uint32_t g_wav_frames, g_wav_since_header;

static void wav_header(void)
{
    uint32_t data = g_wav_frames * 4u, riff = 36u + data, rate = 48000u, bps = 48000u * 4u, fmt = 16;
    uint16_t pcm = 1, ch = 2, align = 4, bits = 16;
    long here = ftell(g_wav);
    fseek(g_wav, 0, SEEK_SET);
    fwrite("RIFF", 1, 4, g_wav); fwrite(&riff, 4, 1, g_wav); fwrite("WAVEfmt ", 1, 8, g_wav);
    fwrite(&fmt, 4, 1, g_wav); fwrite(&pcm, 2, 1, g_wav); fwrite(&ch, 2, 1, g_wav); fwrite(&rate, 4, 1, g_wav);
    fwrite(&bps, 4, 1, g_wav); fwrite(&align, 2, 1, g_wav); fwrite(&bits, 2, 1, g_wav);
    fwrite("data", 1, 4, g_wav); fwrite(&data, 4, 1, g_wav);
    fseek(g_wav, here > 44 ? here : 44, SEEK_SET);
    fflush(g_wav);
}

/* Only ever called from the one thread that mixes (device callback or the
 * paced thread), so the file needs no lock. */
static void mix_out(int16_t *out, uint32_t frames)
{
    dsh_mix(out, frames);
    if (g_wav) {
        fwrite(out, 4, frames, g_wav);
        g_wav_frames += frames;
        g_wav_since_header += frames;
        if (g_wav_since_header >= 48000u) { g_wav_since_header = 0; wav_header(); }
    }
}

static void callback(void *ud, Uint8 *stream, int len)
{
    (void)ud;
    mix_out((int16_t *)stream, (uint32_t)len / 4u);
}

static double now_s(void)
{
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec * 1e-9;
}

/* No device: keep time anyway. The title paces its streams on the play
 * cursor, and one that never moves would stall them. */
static void *paced(void *arg)
{
    static int16_t scratch[2 * 4800];
    double t0 = now_s();
    unsigned long long done = 0;
    (void)arg;
    for (;;) {
        usleep(5000);
        unsigned long long owed = (unsigned long long)((now_s() - t0) * 48000.0) - done;
        while (owed) {
            uint32_t n = owed > 4800u ? 4800u : (uint32_t)owed;
            mix_out(scratch, n);
            owed -= n; done += n;
        }
    }
    return NULL;
}

int dsh_output_start(void)
{
    SDL_AudioSpec want, have;
    if (g_started) return g_started;
    {
        const char *w = getenv("RECOMP_DSOUND_LIFT_WAV");
        if (w && *w && (g_wav = fopen(w, "wb+")) != NULL) {
            fseek(g_wav, 44, SEEK_SET);
            wav_header();
            fprintf(stderr, "[DSOUND-LIFT] output also written to %s\n", w);
        }
    }
    if (SDL_WasInit(SDL_INIT_AUDIO) || SDL_InitSubSystem(SDL_INIT_AUDIO) == 0) {
        SDL_zero(want);
        want.freq = 48000;
        want.format = AUDIO_S16SYS;
        want.channels = 2;
        want.samples = 512;
        want.callback = callback;
        g_dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
        if (g_dev) {
            SDL_PauseAudioDevice(g_dev, 0);
            fprintf(stderr, "[DSOUND-LIFT] output: audio device %u, %d Hz, %d-frame buffers\n",
                    (unsigned)g_dev, have.freq, have.samples);
            return g_started = 1;
        }
        fprintf(stderr, "[DSOUND-LIFT] output: no audio device (%s); pacing by clock\n", SDL_GetError());
    }
    pthread_t t;
    if (pthread_create(&t, NULL, paced, NULL) != 0) return 0;
    pthread_detach(t);
    return g_started = 2;
}
#else
int dsh_output_start(void)
{
    fprintf(stderr, "[DSOUND-LIFT] output: not implemented on Windows yet\n");
    return 0;
}
#endif
