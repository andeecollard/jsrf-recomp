/*
 * apu_wav.c -- the WAV tap on the APU's output. See apu_wav.h.
 */
#include "apu_wav.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
static CRITICAL_SECTION s_lock;
static volatile LONG s_lock_init;
static void wav_lock(void)
{
    /* First use may race between the frame thread and a handler thread. */
    if (InterlockedCompareExchange(&s_lock_init, 1, 0) == 0) {
        InitializeCriticalSection(&s_lock);
        InterlockedExchange(&s_lock_init, 2);
    }
    while (s_lock_init != 2)
        Sleep(0);
    EnterCriticalSection(&s_lock);
}
static void wav_unlock(void) { LeaveCriticalSection(&s_lock); }
/* Only called with a file open, so the lock is initialised. */
static int  wav_trylock(void) { return TryEnterCriticalSection(&s_lock) != 0; }
static void wav_nap(void)     { Sleep(1); }
static uint64_t wav_ms(void)  { return GetTickCount64(); }
#else
#include <pthread.h>
#include <time.h>
static pthread_mutex_t s_mutex = PTHREAD_MUTEX_INITIALIZER;
static void wav_lock(void)   { pthread_mutex_lock(&s_mutex); }
static void wav_unlock(void) { pthread_mutex_unlock(&s_mutex); }
static int  wav_trylock(void) { return pthread_mutex_trylock(&s_mutex) == 0; }
static void wav_nap(void)
{
    struct timespec ts = { 0, 1000000 };
    nanosleep(&ts, NULL);
}
static uint64_t wav_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}
#endif

#define FRAME_BYTES (APU_WAV_CHANNELS * 2)

static FILE    *s_fp;
static uint64_t s_frames;        /* sample frames written */
static uint64_t s_cap;           /* 0 = uncapped */
static uint64_t s_next_refresh;  /* s_frames at which the header is rewritten */
static int      s_hooks;         /* atexit / ctrl handler registered */

static void put_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

/* Write the 44-byte header for the current size at offset 0, then return to
 * the end. Called with the lock held. */
static void write_header(void)
{
    uint8_t h[44];
    uint64_t bytes = s_frames * FRAME_BYTES;
    uint32_t data = bytes > 0xFFFFFFD3u ? 0xFFFFFFD3u : (uint32_t)bytes;

    memcpy(h, "RIFF", 4);
    put_le32(h + 4, 36 + data);
    memcpy(h + 8, "WAVEfmt ", 8);
    put_le32(h + 16, 16);
    h[20] = 1; h[21] = 0;                                   /* PCM        */
    h[22] = APU_WAV_CHANNELS; h[23] = 0;
    put_le32(h + 24, APU_WAV_RATE);
    put_le32(h + 28, APU_WAV_RATE * FRAME_BYTES);
    h[32] = FRAME_BYTES; h[33] = 0;                         /* block align */
    h[34] = 16; h[35] = 0;                                  /* bits        */
    memcpy(h + 36, "data", 4);
    put_le32(h + 40, data);

    fseek(s_fp, 0, SEEK_SET);
    fwrite(h, 1, sizeof h, s_fp);
    fseek(s_fp, 0, SEEK_END);
    fflush(s_fp);
}

static void close_locked(void)
{
    if (!s_fp)
        return;
    write_header();
    fclose(s_fp);
    s_fp = NULL;
    fprintf(stderr, "[AUDIO-WAV] closed: %llu samples (%.2f s)\n",
            (unsigned long long)s_frames, (double)s_frames / APU_WAV_RATE);
}

void apu_wav_close(void)
{
    wav_lock();
    close_locked();
    wav_unlock();
}

int apu_wav_close_nowait(int wait_ms)
{
    uint64_t deadline;

    if (!s_fp)
        return 1;
    /* Against the clock, not a count of naps: Sleep(1) lasts a whole
     * scheduler tick (about 15.6 ms) on a default Windows timer. */
    deadline = wav_ms() + (uint64_t)(wait_ms > 0 ? wait_ms : 0);
    for (;;) {
        if (wav_trylock()) {
            close_locked();
            wav_unlock();
            return 1;
        }
        if (wav_ms() >= deadline)
            return 0;
        wav_nap();
    }
}

static void wav_atexit(void) { apu_wav_close(); }

#if defined(_WIN32)
static BOOL WINAPI wav_ctrl(DWORD type)
{
    (void)type;
    apu_wav_close();
    return FALSE;                       /* let the next handler run */
}
#endif

int apu_wav_open(const char *path, double max_secs)
{
    int ok = 0;

    if (!path || !*path)
        return 0;
    wav_lock();
    if (!s_fp) {
        s_fp = fopen(path, "wb");
        if (s_fp) {
            s_frames = 0;
            s_cap = max_secs > 0 ? (uint64_t)(max_secs * APU_WAV_RATE + 0.5) : 0;
            s_next_refresh = APU_WAV_RATE;
            write_header();
            ok = 1;
            fprintf(stderr, "[AUDIO-WAV] writing %s (48000 Hz s16 stereo%s)\n",
                    path, s_cap ? ", capped" : "");
        } else {
            fprintf(stderr, "[AUDIO-WAV] cannot open %s\n", path);
        }
    }
    if (ok && !s_hooks) {
        s_hooks = 1;
        atexit(wav_atexit);
#if defined(_WIN32)
        SetConsoleCtrlHandler(wav_ctrl, TRUE);
#endif
    }
    wav_unlock();
    return ok;
}

void apu_wav_write(const int16_t *samples, int count)
{
    uint8_t buf[1024 * FRAME_BYTES];

    if (!s_fp || !samples || count <= 0)        /* unlocked fast path */
        return;
    wav_lock();
    while (s_fp && count > 0) {
        int n = count > 1024 ? 1024 : count, i;
        if (s_cap && s_frames + (uint64_t)n > s_cap)
            n = (int)(s_cap - s_frames);
        /* Little-endian on disk whatever the host. */
        for (i = 0; i < n * APU_WAV_CHANNELS; i++) {
            buf[2 * i]     = (uint8_t)samples[i];
            buf[2 * i + 1] = (uint8_t)((uint16_t)samples[i] >> 8);
        }
        fwrite(buf, FRAME_BYTES, (size_t)n, s_fp);
        s_frames += (uint64_t)n;
        samples += n * APU_WAV_CHANNELS;
        count -= n;
        if (s_cap && s_frames >= s_cap) {
            close_locked();
            break;
        }
        if (s_frames >= s_next_refresh) {
            write_header();
            s_next_refresh = s_frames + APU_WAV_RATE;
        }
    }
    wav_unlock();
}

int apu_wav_is_open(void)
{
    return s_fp != NULL;
}
