#include "apu_sdl2.h"

#if !defined(_WIN32)

#include <SDL.h>
#include <stdio.h>

#define APU_SAMPLE_RATE 48000
#define APU_CHANNELS 2
#define APU_MAX_QUEUE_BYTES (APU_SAMPLE_RATE * APU_CHANNELS * (int)sizeof(int16_t) / 10)

static SDL_AudioDeviceID g_device;
static unsigned g_dropped_batches;
static unsigned g_nonzero_reports;

int apu_sdl2_init(void)
{
    SDL_AudioSpec wanted;
    SDL_AudioSpec obtained;

    if (g_device)
        return 1;
    if (!SDL_WasInit(SDL_INIT_AUDIO) && SDL_InitSubSystem(SDL_INIT_AUDIO) < 0) {
        fprintf(stderr, "[APU-SDL] SDL audio initialization failed: %s\n",
                SDL_GetError());
        return 0;
    }

    SDL_zero(wanted);
    wanted.freq = APU_SAMPLE_RATE;
    wanted.format = AUDIO_S16SYS;
    wanted.channels = APU_CHANNELS;
    wanted.samples = 512;
    g_device = SDL_OpenAudioDevice(NULL, 0, &wanted, &obtained, 0);
    if (!g_device) {
        fprintf(stderr, "[APU-SDL] output open failed: %s\n", SDL_GetError());
        return 0;
    }

    SDL_PauseAudioDevice(g_device, 0);
    fprintf(stderr, "[APU-SDL] output ready: %d Hz, %d channel, format 0x%04X\n",
            obtained.freq, obtained.channels, obtained.format);
    return 1;
}

void apu_sdl2_shutdown(void)
{
    if (!g_device)
        return;
    SDL_ClearQueuedAudio(g_device);
    SDL_CloseAudioDevice(g_device);
    g_device = 0;
}

int apu_sdl2_is_active(void)
{
    return g_device != 0;
}

int apu_sdl2_submit_samples(const int16_t *samples, int sample_frames)
{
    unsigned bytes;
    int peak = 0;

    if (!g_device || !samples || sample_frames <= 0)
        return 0;
    bytes = (unsigned)sample_frames * APU_CHANNELS * sizeof(int16_t);
    for (int i = 0; i < sample_frames * APU_CHANNELS; i++) {
        int magnitude = samples[i] < 0 ? -(int)samples[i] : samples[i];
        if (magnitude > peak)
            peak = magnitude;
    }
    if (peak && g_nonzero_reports++ < 4)
        fprintf(stderr, "[APU-SDL] queued audible batch: %d frames, peak %d\n",
                sample_frames, peak);

    /* Prefer a bounded discontinuity to seconds of stale audio after a host
     * stall or debugger stop. Normal paced playback remains far below this. */
    if (SDL_GetQueuedAudioSize(g_device) > APU_MAX_QUEUE_BYTES) {
        SDL_ClearQueuedAudio(g_device);
        if (g_dropped_batches++ < 4)
            fprintf(stderr, "[APU-SDL] cleared overfull output queue\n");
    }
    if (SDL_QueueAudio(g_device, samples, bytes) < 0) {
        fprintf(stderr, "[APU-SDL] queue failed: %s\n", SDL_GetError());
        return 0;
    }
    return 1;
}

#else

int apu_sdl2_init(void) { return 0; }
void apu_sdl2_shutdown(void) {}
int apu_sdl2_is_active(void) { return 0; }
int apu_sdl2_submit_samples(const int16_t *samples, int sample_frames)
{
    (void)samples;
    (void)sample_frames;
    return 0;
}

#endif
