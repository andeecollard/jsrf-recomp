/* The host DSOUND's output: dsh_mix pulled by the audio device, or by a
 * real-time thread when there is no device. See dsound_host.h.
 *
 *   macOS / Linux: an SDL audio device in callback mode.
 *   Windows:       an XAudio2 source voice of its own, kept a few buffers
 *                  ahead by a pump thread (G48 §2f). Its own XAudio2 instance,
 *                  so it never shares a voice with the APU model's backend.
 *   Either, when no device opens (a silenced harness run): a thread mixes by
 *   the clock, so cursors and status still advance as they would with one. */
#include "dsound_host.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define OUT_RATE 48000u

/* ---- the WAV tee (RECOMP_DSOUND_LIFT_WAV=<path>) ------------------------
 * Every frame the lift mixes is also written to a 48 kHz stereo WAV, so a
 * silenced harness run can be checked by script -- clipping, silence,
 * dropouts -- without playing anything aloud. The header is rewritten every
 * second of audio, so a killed run still leaves a readable file. Only the one
 * thread that mixes ever calls mix_out, so the file needs no lock. */
static FILE *g_wav;
static uint32_t g_wav_frames, g_wav_since_header;

static void wav_header(void)
{
    uint32_t data = g_wav_frames * 4u, riff = 36u + data, rate = OUT_RATE, bps = OUT_RATE * 4u, fmt = 16;
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

static void wav_open(void)
{
    const char *w = getenv("RECOMP_DSOUND_LIFT_WAV");
    if (w && *w && (g_wav = fopen(w, "wb+")) != NULL) {
        fseek(g_wav, 44, SEEK_SET);
        wav_header();
        fprintf(stderr, "[DSOUND-LIFT] output also written to %s\n", w);
    }
}

static void mix_out(int16_t *out, uint32_t frames)
{
    dsh_mix(out, frames);
    if (g_wav) {
        fwrite(out, 4, frames, g_wav);
        g_wav_frames += frames;
        g_wav_since_header += frames;
        if (g_wav_since_header >= OUT_RATE) { g_wav_since_header = 0; wav_header(); }
    }
}

static int g_started;

#if !defined(_WIN32)
#include <SDL.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>

static SDL_AudioDeviceID g_dev;

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
static void sleep_ms(unsigned ms) { usleep(ms * 1000u); }

static void *paced(void *arg);
static int start_paced(void)
{
    pthread_t t;
    if (pthread_create(&t, NULL, paced, NULL) != 0) return 0;
    pthread_detach(t);
    return 2;
}

static int start_device(void)
{
    SDL_AudioSpec want, have;
    if (!SDL_WasInit(SDL_INIT_AUDIO) && SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) return 0;
    SDL_zero(want);
    want.freq = (int)OUT_RATE;
    want.format = AUDIO_S16SYS;
    want.channels = 2;
    want.samples = 512;
    want.callback = callback;
    g_dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (!g_dev) {
        fprintf(stderr, "[DSOUND-LIFT] output: no audio device (%s); pacing by clock\n", SDL_GetError());
        return 0;
    }
    SDL_PauseAudioDevice(g_dev, 0);
    fprintf(stderr, "[DSOUND-LIFT] output: audio device %u, %d Hz, %d-frame buffers\n",
            (unsigned)g_dev, have.freq, have.samples);
    return 1;
}
#else
#define COBJMACROS
#include <windows.h>
#include <xaudio2.h>

#define XA_FRAMES 1024u     /* ~21 ms per buffer */
#define XA_BUFS   8u
#define XA_AHEAD  4u        /* ~85 ms queued */

static IXAudio2 *g_xa;
static IXAudio2MasteringVoice *g_xa_master;
static IXAudio2SourceVoice *g_xa_src;
static int16_t g_xa_buf[XA_BUFS][XA_FRAMES * 2];

static double now_s(void)
{
    static LARGE_INTEGER f;
    LARGE_INTEGER c;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart / (double)f.QuadPart;
}
static void sleep_ms(unsigned ms) { Sleep(ms); }

static DWORD WINAPI xa_pump(LPVOID arg)
{
    unsigned idx = 0;
    (void)arg;
    for (;;) {
        XAUDIO2_VOICE_STATE st;
        IXAudio2SourceVoice_GetState(g_xa_src, &st, XAUDIO2_VOICE_NOSAMPLESPLAYED);
        while (st.BuffersQueued < XA_AHEAD) {
            XAUDIO2_BUFFER b;
            mix_out(g_xa_buf[idx], XA_FRAMES);
            memset(&b, 0, sizeof b);
            b.AudioBytes = XA_FRAMES * 4u;
            b.pAudioData = (const BYTE *)g_xa_buf[idx];
            if (FAILED(IXAudio2SourceVoice_SubmitSourceBuffer(g_xa_src, &b, NULL))) break;
            idx = (idx + 1u) % XA_BUFS;
            st.BuffersQueued++;
        }
        Sleep(5);
    }
    return 0;
}

static DWORD WINAPI paced_win(LPVOID arg);
static int start_paced(void)
{
    HANDLE t = CreateThread(NULL, 0, paced_win, NULL, 0, NULL);
    if (!t) return 0;
    CloseHandle(t);
    return 2;
}

static int start_device(void)
{
    WAVEFORMATEX wfx;
    HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) return 0;
    if (FAILED(XAudio2Create(&g_xa, 0, XAUDIO2_DEFAULT_PROCESSOR)) || !g_xa) goto fail;
    if (FAILED(IXAudio2_CreateMasteringVoice(g_xa, &g_xa_master, 2, OUT_RATE, 0, NULL, NULL, 0))) goto fail;
    memset(&wfx, 0, sizeof wfx);
    wfx.wFormatTag = WAVE_FORMAT_PCM;
    wfx.nChannels = 2;
    wfx.nSamplesPerSec = OUT_RATE;
    wfx.wBitsPerSample = 16;
    wfx.nBlockAlign = 4;
    wfx.nAvgBytesPerSec = OUT_RATE * 4u;
    if (FAILED(IXAudio2_CreateSourceVoice(g_xa, &g_xa_src, &wfx, 0, XAUDIO2_DEFAULT_FREQ_RATIO, NULL, NULL, NULL))) goto fail;
    if (FAILED(IXAudio2SourceVoice_Start(g_xa_src, 0, XAUDIO2_COMMIT_NOW))) goto fail;
    {
        HANDLE t = CreateThread(NULL, 0, xa_pump, NULL, 0, NULL);
        if (!t) goto fail;
        SetThreadPriority(t, THREAD_PRIORITY_TIME_CRITICAL);
        CloseHandle(t);
    }
    fprintf(stderr, "[DSOUND-LIFT] output: XAudio2 voice, %u Hz, %u x %u-frame buffers ahead\n",
            OUT_RATE, XA_AHEAD, XA_FRAMES);
    return 1;
fail:
    fprintf(stderr, "[DSOUND-LIFT] output: XAudio2 unavailable; pacing by clock\n");
    return 0;
}
#endif

/* No device: keep time anyway. The title paces its streams on the play
 * cursor, and one that never moves would stall them. */
static void paced_loop(void)
{
    static int16_t scratch[2 * 4800];
    double t0 = now_s();
    unsigned long long done = 0;
    for (;;) {
        sleep_ms(5);
        unsigned long long owed = (unsigned long long)((now_s() - t0) * (double)OUT_RATE) - done;
        while (owed) {
            uint32_t n = owed > 4800u ? 4800u : (uint32_t)owed;
            mix_out(scratch, n);
            owed -= n; done += n;
        }
    }
}
#if !defined(_WIN32)
static void *paced(void *arg) { (void)arg; paced_loop(); return NULL; }
#else
static DWORD WINAPI paced_win(LPVOID arg) { (void)arg; paced_loop(); return 0; }
#endif

int dsh_output_start(void)
{
    if (g_started) return g_started;
    wav_open();
    g_started = start_device();
    if (!g_started) g_started = start_paced();
    return g_started;
}
