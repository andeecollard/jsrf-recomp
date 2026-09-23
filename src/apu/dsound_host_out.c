/* The host DSOUND's output: dsh_mix pulled by the audio device, or by a
 * real-time thread when there is no device. See dsound_host.h. */
#include "dsound_host.h"

#include <stdio.h>

#if !defined(_WIN32)
#include <SDL.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>

static SDL_AudioDeviceID g_dev;
static int g_started;

static void callback(void *ud, Uint8 *stream, int len)
{
    (void)ud;
    dsh_mix((int16_t *)stream, (uint32_t)len / 4u);
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
            dsh_mix(scratch, n);
            owed -= n; done += n;
        }
    }
    return NULL;
}

int dsh_output_start(void)
{
    SDL_AudioSpec want, have;
    if (g_started) return g_started;
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
