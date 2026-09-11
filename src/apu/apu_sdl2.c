#include "apu_sdl2.h"

#if !defined(_WIN32)

#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>

#define APU_SAMPLE_RATE 48000
#define APU_CHANNELS 2
/* One SDL callback buffer: 512 frames, the unit the device drains in. */
#define APU_DEVICE_BUFFER_BYTES (512 * APU_CHANNELS * (int)sizeof(int16_t))
/* The cushion, in device buffers. Four is ~43 ms: enough that the producer
 * missing a deadline costs latency instead of silence, and small enough that
 * the added output delay stays well under a video frame and a half. */
#define APU_PRIME_BUFFERS 4
/* The drop threshold has to sit clear above the cushion, or the cushion itself
 * would look like a runaway queue and be cleared. Baseline runs peaked near
 * 101 ms with no cushion at all, so 200 ms leaves the natural variation room
 * on top of the prime while still bounding latency after a host stall. */
#define APU_MAX_QUEUE_BYTES (APU_SAMPLE_RATE * APU_CHANNELS * (int)sizeof(int16_t) / 5)

static SDL_AudioDeviceID g_device;
/* The device's actual spec, not the requested one: every threshold below is
 * derived from what SDL really gave us, so a device that rounds the buffer
 * size does not silently invalidate the arithmetic. */
static SDL_AudioSpec g_spec;
static unsigned g_dev_buf_bytes = (unsigned)APU_DEVICE_BUFFER_BYTES;
/* The device is created paused and stays that way until the queue holds the
 * cushion. Starting playback against an empty queue is what put holes in the
 * intro: the device drains from the first callback while the producer has
 * banked nothing, so every early submit is a race the producer loses. */
static int g_primed;
/* Delivery timing, which the sample data cannot show.
 *
 * A dump of submitted buffers is a concatenation with no clock attached: a
 * producer that stalls and then submits looks identical to one that never
 * stalled. What decides whether the device runs out is the interval between
 * submits measured against the cushion -- if a gap exceeds what is queued, the
 * device plays silence inside that gap no matter how clean the samples are. */
static Uint64 g_last_submit_ticks;
static double g_tick_ms;
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

    g_spec = obtained;
    if (obtained.samples)
        g_dev_buf_bytes = (unsigned)obtained.samples * obtained.channels
                        * (unsigned)sizeof(int16_t);
    /* Deliberately NOT unpaused here -- see g_primed. */
    fprintf(stderr, "[APU-SDL] output ready: %d Hz, %d channel, format 0x%04X,"
            " samples=%u -> device buffer %u bytes (%.2f ms)\n",
            obtained.freq, obtained.channels, obtained.format,
            (unsigned)obtained.samples, g_dev_buf_bytes,
            g_dev_buf_bytes / 192.0);
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

/* Production accounting, read by mcpx_apu_pacing_report.
 *
 * The queue depth is the only thing that says whether the APU is producing at
 * playback rate: a device consuming 48000 frames a second against a producer
 * doing the same holds a roughly constant queue, and any mismatch shows up
 * here as a trend long before it becomes an audible drop. */
unsigned long g_apu_sdl_batches;
unsigned long g_apu_sdl_frames;
unsigned long g_apu_sdl_clears;

/* Starvation accounting -- the other end of the queue.
 *
 * The counters above watch the queue filling, because a producer running fast
 * drifts upward and is eventually cleared. They say nothing about it running
 * dry, and that is the failure that is audible: when SDL reaches the end of
 * the queued data it plays silence, so a producer late by one device buffer
 * puts a hole in the output. Chopped, skipping audio is that hole repeating.
 *
 * A depth of zero read here is evidence, not proof, of an underrun: the device
 * may have consumed everything an instant before this submit topped it up. The
 * number to trust is the minimum across a run -- that is the headroom, and if
 * it reaches zero the margin is gone whether or not a reading caught the gap. */
/* Raw capture of exactly what is handed to the device.
 *
 * The starvation counters say the queue ran dry; they cannot say whether the
 * mixer had anything to put in it. Those are different faults with different
 * fixes -- an output-side priming problem versus the source (disc/ADX) failing
 * to deliver -- and they are told apart by looking at the samples themselves.
 * A stream that is continuous here but chopped when heard is an output fault;
 * one that already contains runs of silence was broken before it arrived.
 *
 * S16 stereo at 48 kHz, headerless, so it can be read straight into an
 * analysis script. Opt-in via RECOMP_AUDIO_DUMP=<path>. */
static FILE *g_dump;
static int g_dump_tried;

static void apu_dump_open(void)
{
    const char *path;
    if (g_dump_tried) return;
    g_dump_tried = 1;
    path = getenv("RECOMP_AUDIO_DUMP");
    if (!path) return;
    g_dump = fopen(path, "wb");
    fprintf(stderr, "[APU-SDL] audio dump %s: %s\n",
            g_dump ? "open" : "FAILED", path);
}

/* Depth distribution at submit, bucketed in device buffers, so the shape of
 * steady state is visible rather than just its extremes. */
unsigned long g_apu_sdl_depth_hist[6];   /* 0, <1, 1-2, 2-4, 4-8, 8+ buffers */
unsigned long g_apu_sdl_max_bytes;
unsigned long g_apu_sdl_max_gap_us;   /* longest interval between submits */
unsigned long g_apu_sdl_gaps_over_cushion; /* intervals longer than the queue */
unsigned long g_apu_sdl_prime_bytes;
unsigned long g_apu_sdl_reprimes;
unsigned long g_apu_sdl_starved;    /* depth below one device buffer */
unsigned long g_apu_sdl_empty;      /* depth exactly zero */
unsigned long g_apu_sdl_min_bytes = (unsigned long)-1;

unsigned long apu_sdl2_queued_bytes(void)
{
    return g_device ? (unsigned long)SDL_GetQueuedAudioSize(g_device) : 0;
}

int apu_sdl2_submit_samples(const int16_t *samples, int sample_frames)
{
    unsigned bytes;
    int peak = 0;

    if (!g_device || !samples || sample_frames <= 0)
        return 0;
    g_apu_sdl_batches++;
    g_apu_sdl_frames += (unsigned long)sample_frames;
    apu_dump_open();
    if (g_dump)
        fwrite(samples, sizeof(int16_t) * APU_CHANNELS,
               (size_t)sample_frames, g_dump);
    bytes = (unsigned)sample_frames * APU_CHANNELS * sizeof(int16_t);
    for (int i = 0; i < sample_frames * APU_CHANNELS; i++) {
        int magnitude = samples[i] < 0 ? -(int)samples[i] : samples[i];
        if (magnitude > peak)
            peak = magnitude;
    }
    if (peak && g_nonzero_reports++ < 4)
        fprintf(stderr, "[APU-SDL] queued audible batch: %d frames, peak %d\n",
                sample_frames, peak);


    /* Read the depth once, before topping it up: what is left here is what the
     * device still has to play, which is the quantity that decides whether it
     * runs out. */
    /* Interval since the previous submit, against what was queued at the time.
     * This is the measurement the sample dump cannot make. */
    if (g_primed) {
        Uint64 now = SDL_GetPerformanceCounter();
        if (!g_tick_ms)
            g_tick_ms = 1000.0 / (double)SDL_GetPerformanceFrequency();
        if (g_last_submit_ticks) {
            double gap_ms = (double)(now - g_last_submit_ticks) * g_tick_ms;
            unsigned long gap_us = (unsigned long)(gap_ms * 1000.0);
            unsigned long queued_ms =
                (unsigned long)(SDL_GetQueuedAudioSize(g_device) / 192);
            if (gap_us > g_apu_sdl_max_gap_us)
                g_apu_sdl_max_gap_us = gap_us;
            if (gap_ms > (double)queued_ms)
                g_apu_sdl_gaps_over_cushion++;
        }
        g_last_submit_ticks = now;
    } else {
        g_last_submit_ticks = SDL_GetPerformanceCounter();
    }

    {
        unsigned long depth = (unsigned long)SDL_GetQueuedAudioSize(g_device);
        /* Only meaningful once playing. Before the first unpause the queue is
         * supposed to be shallow -- that is the cushion being filled, not
         * starvation -- so counting it would guarantee a false positive. */
        if (g_primed) {
            if (depth < g_apu_sdl_min_bytes)
                g_apu_sdl_min_bytes = depth;
            if (depth == 0)
                g_apu_sdl_empty++;
            if (depth > g_apu_sdl_max_bytes)
                g_apu_sdl_max_bytes = depth;
            if (depth < g_dev_buf_bytes)
                g_apu_sdl_starved++;
            {
                unsigned long b = g_dev_buf_bytes ? depth / g_dev_buf_bytes : 0;
                unsigned idx = depth == 0 ? 0
                             : b == 0 ? 1 : b < 2 ? 2 : b < 4 ? 3 : b < 8 ? 4 : 5;
                g_apu_sdl_depth_hist[idx]++;
            }

            /* A collapse to empty means the cushion is gone and every
             * subsequent callback plays silence until the producer catches up.
             * Re-pausing rebuilds it in one bounded refill instead of leaving
             * a run of holes. Counted, because needing this repeatedly would
             * mean the cushion is too small rather than merely unlucky. */
            if (depth == 0) {
                SDL_PauseAudioDevice(g_device, 1);
                g_primed = 0;
                g_apu_sdl_reprimes++;
            }
        }
    }

    /* Prefer a bounded discontinuity to seconds of stale audio after a host
     * stall or debugger stop. Normal paced playback remains far below this. */
    if (SDL_GetQueuedAudioSize(g_device) > APU_MAX_QUEUE_BYTES) {
        SDL_ClearQueuedAudio(g_device);
        g_apu_sdl_clears++;
        if (g_dropped_batches++ < 4)
            fprintf(stderr, "[APU-SDL] cleared overfull output queue\n");
    }
    if (SDL_QueueAudio(g_device, samples, bytes) < 0) {
        fprintf(stderr, "[APU-SDL] queue failed: %s\n", SDL_GetError());
        return 0;
    }

    /* Release the device only once the cushion is actually in the queue. The
     * threshold is a byte count derived from the device's own buffer size, so
     * there is no sleep here and nothing in the emulated audio path is
     * delayed -- the producer runs at its own rate throughout and this only
     * decides when the consumer is allowed to start. */
    if (!g_primed) {
        unsigned long depth = (unsigned long)SDL_GetQueuedAudioSize(g_device);
        if (depth >= (unsigned long)g_dev_buf_bytes * APU_PRIME_BUFFERS) {
            if (!g_apu_sdl_prime_bytes)
                g_apu_sdl_prime_bytes = depth;
            g_primed = 1;
            SDL_PauseAudioDevice(g_device, 0);
        }
    }
    return 1;
}

#else

int apu_sdl2_init(void) { return 0; }
void apu_sdl2_shutdown(void) {}
int apu_sdl2_is_active(void) { return 0; }
unsigned long g_apu_sdl_batches;
unsigned long g_apu_sdl_frames;
unsigned long g_apu_sdl_starved;
unsigned long g_apu_sdl_empty;
unsigned long g_apu_sdl_min_bytes;
unsigned long g_apu_sdl_max_bytes;
unsigned long g_apu_sdl_depth_hist[6];
unsigned long g_apu_sdl_prime_bytes;
unsigned long g_apu_sdl_reprimes;
unsigned long g_apu_sdl_clears;

unsigned long apu_sdl2_queued_bytes(void) { return 0; }

int apu_sdl2_submit_samples(const int16_t *samples, int sample_frames)
{
    (void)samples;
    (void)sample_frames;
    return 0;
}

#endif
