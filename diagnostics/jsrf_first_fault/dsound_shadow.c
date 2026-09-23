/* G48 phase 3: the host DSOUND model in SHADOW of the title's own DSOUND.
 *
 * Copied into a scratch gen as recomp_zz_dsound_shadow.c by
 * stage_dsound_census.py --shadow. Armed by RECOMP_DSOUND_CENSUS=1 together
 * with RECOMP_DSOUND_SHADOW=1.
 *
 * The original DSOUND still runs and still drives the APU model. After each
 * call it returns, the census wrapper hands the call's arguments and result to
 * dss_after(), which replays it into src/apu/dsound_host.c. A thread mixes the
 * model in real time -- into a WAV file if RECOMP_DSOUND_SHADOW_WAV names one,
 * otherwise into scratch -- so its cursors and status move as a device would.
 *
 * What it measures, the questions phase 3 must answer before replacement:
 *   - every buffer the title creates is one the model accepts;
 *   - GetStatus: the model's answer against DSOUND's, on every call;
 *   - GetCurrentPosition: the play-cursor difference, in milliseconds.
 * Read-only with respect to the title: nothing it computes reaches the guest. */
#define RECOMP_GENERATED_CODE
#include "recomp_funcs.h"
#include "dsound_host.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define OUT_RATE 48000u

static int s_on = -1;
static FILE *s_wav;
static uint32_t s_wav_frames;
static pthread_mutex_t s_wav_m = PTHREAD_MUTEX_INITIALIZER;

/* Buffers whose memory DSOUND owns: base learned from the first Lock. */
#define OWNED_MAX 64
static struct { uint32_t handle, bytes, base; } s_owned[OWNED_MAX];
static unsigned s_owned_n;

static _Atomic unsigned long s_status_calls, s_status_agree, s_status_model_playing_only,
                             s_status_dsound_playing_only, s_status_other, s_status_unknown_buf;
static _Atomic unsigned long s_pos_calls, s_pos_unknown_buf, s_pos_hist[8];
static _Atomic unsigned long s_create_ok, s_create_refused, s_calls;
static unsigned s_logged_disagree;

static const uint8_t *mem(uint32_t va, uint32_t len)
{
    if (!va || va + len < va) return NULL;
    return (const uint8_t *)((uintptr_t)va + g_xbox_mem_offset);
}

static void wav_header(FILE *f, uint32_t frames)
{
    uint32_t data = frames * 4u, riff = 36u + data, rate = OUT_RATE, bps = OUT_RATE * 4u, fmt = 16;
    uint16_t pcm = 1, ch = 2, align = 4, bits = 16;
    fseek(f, 0, SEEK_SET);
    fwrite("RIFF", 1, 4, f); fwrite(&riff, 4, 1, f); fwrite("WAVEfmt ", 1, 8, f);
    fwrite(&fmt, 4, 1, f); fwrite(&pcm, 2, 1, f); fwrite(&ch, 2, 1, f); fwrite(&rate, 4, 1, f);
    fwrite(&bps, 4, 1, f); fwrite(&align, 2, 1, f); fwrite(&bits, 2, 1, f);
    fwrite("data", 1, 4, f); fwrite(&data, 4, 1, f);
    fseek(f, 0, SEEK_END);
}

static double now_s(void)
{
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec * 1e-9;
}

/* Real-time pacing: mix whatever the wall clock says is owed, in 5 ms steps. */
static void *mixer_thread(void *arg)
{
    (void)arg;
    static int16_t buf[2 * 4800];
    double t0 = now_s();
    uint64_t done = 0;
    for (;;) {
        usleep(5000);
        uint64_t owed = (uint64_t)((now_s() - t0) * OUT_RATE) - done;
        while (owed) {
            uint32_t n = owed > 4800u ? 4800u : (uint32_t)owed;
            dsh_mix(buf, n);
            if (s_wav) {
                pthread_mutex_lock(&s_wav_m);
                fwrite(buf, 4, n, s_wav);
                s_wav_frames += n;
                pthread_mutex_unlock(&s_wav_m);
            }
            owed -= n; done += n;
        }
    }
    return NULL;
}

static void report(const char *why)
{
    dsh_stats st; dsh_get_stats(&st);
    fprintf(stderr, "[DSOUND-SHADOW] %s calls=%lu buffers=%lu created=%lu refused=%lu released=%lu"
            " plays=%lu stops=%lu playing_now=%lu missing_data=%lu owned=%u\n",
            why, (unsigned long)s_calls, st.buffers, (unsigned long)s_create_ok,
            (unsigned long)s_create_refused, st.released, st.plays, st.stops, st.playing,
            st.missing_data, s_owned_n);
    fprintf(stderr, "[DSOUND-SHADOW] GetStatus calls=%lu agree=%lu model-only-playing=%lu"
            " dsound-only-playing=%lu other=%lu unknown-buffer=%lu\n",
            (unsigned long)s_status_calls, (unsigned long)s_status_agree,
            (unsigned long)s_status_model_playing_only, (unsigned long)s_status_dsound_playing_only,
            (unsigned long)s_status_other, (unsigned long)s_status_unknown_buf);
    fprintf(stderr, "[DSOUND-SHADOW] GetCurrentPosition calls=%lu unknown-buffer=%lu |model-dsound| ms:"
            " <1:%lu <5:%lu <10:%lu <20:%lu <50:%lu <100:%lu <500:%lu >=500:%lu\n",
            (unsigned long)s_pos_calls, (unsigned long)s_pos_unknown_buf,
            (unsigned long)s_pos_hist[0], (unsigned long)s_pos_hist[1], (unsigned long)s_pos_hist[2],
            (unsigned long)s_pos_hist[3], (unsigned long)s_pos_hist[4], (unsigned long)s_pos_hist[5],
            (unsigned long)s_pos_hist[6], (unsigned long)s_pos_hist[7]);
    if (s_wav) {
        pthread_mutex_lock(&s_wav_m);
        wav_header(s_wav, s_wav_frames);
        fflush(s_wav);
        pthread_mutex_unlock(&s_wav_m);
    }
    fflush(stderr);
}
static void at_exit(void) { report("exit"); }

static int armed(void)
{
    if (s_on < 0) {
        const char *v = getenv("RECOMP_DSOUND_SHADOW");
        s_on = v && strcmp(v, "1") == 0;
        if (s_on) {
            const char *w = getenv("RECOMP_DSOUND_SHADOW_WAV");
            dsh_init(mem, OUT_RATE);
            dsh_reset();
            if (w && *w && (s_wav = fopen(w, "wb+")) != NULL) wav_header(s_wav, 0);
            pthread_t t; pthread_create(&t, NULL, mixer_thread, NULL); pthread_detach(t);
            atexit(at_exit);
            fprintf(stderr, "[DSOUND-SHADOW] armed; wav=%s\n", s_wav ? w : "(none)");
        }
    }
    return s_on;
}

/* Format lookup for the position comparison: kept beside the owned table. */
#define FMT_MAX 4096
static struct { uint32_t handle; uint32_t bytes_per_ms_x100; } s_fmt[FMT_MAX];
static void fmt_note(uint32_t h, const dsh_format *f)
{
    uint32_t k = (h >> 3) % FMT_MAX;
    uint32_t bpms = f->tag == DSH_TAG_XBOX_ADPCM
                        ? (uint32_t)((uint64_t)f->rate * f->block_align * 100u / 65u / 1000u)
                        : f->rate * f->block_align / 10u;
    for (unsigned i = 0; i < FMT_MAX; ++i, k = (k + 1) % FMT_MAX)
        if (!s_fmt[k].handle || s_fmt[k].handle == h) { s_fmt[k].handle = h; s_fmt[k].bytes_per_ms_x100 = bpms; return; }
}
static uint32_t fmt_bpms(uint32_t h)
{
    uint32_t k = (h >> 3) % FMT_MAX;
    for (unsigned i = 0; i < FMT_MAX && s_fmt[k].handle; ++i, k = (k + 1) % FMT_MAX)
        if (s_fmt[k].handle == h) return s_fmt[k].bytes_per_ms_x100;
    return 0;
}

void dss_after(const char *name, const uint32_t *a, uint32_t eax)
{
    if (!armed()) return;
    atomic_fetch_add(&s_calls, 1);
    const char *n = name;
    if (!strcmp(n, "IDirectSound_CreateSoundBuffer")) {
        if (eax) return;
        uint32_t d = a[1], wf = MEM32(d + 12u), h = MEM32(a[2]);
        dsh_format f = { MEM16(wf), MEM16(wf + 2u), MEM32(wf + 4u), MEM16(wf + 14u), MEM16(wf + 12u) };
        uint32_t bytes = MEM32(d + 8u);
        if (dsh_buffer_create(h, &f, 0, bytes) == 0) {
            atomic_fetch_add(&s_create_ok, 1);
            fmt_note(h, &f);
            if (bytes && s_owned_n < OWNED_MAX) {
                s_owned[s_owned_n].handle = h; s_owned[s_owned_n].bytes = bytes; s_owned[s_owned_n].base = 0;
                s_owned_n++;
            }
        } else {
            atomic_fetch_add(&s_create_refused, 1);
            fprintf(stderr, "[DSOUND-SHADOW] REFUSED buffer %08X tag=%u ch=%u rate=%u bits=%u align=%u bytes=%u\n",
                    h, f.tag, f.channels, f.rate, f.bits, f.block_align, bytes);
        }
    } else if (!strcmp(n, "IDirectSoundBuffer_SetBufferData")) {
        dsh_set_buffer_data(a[0], a[1], a[2]);
    } else if (!strcmp(n, "IDirectSoundBuffer_Lock")) {
        uint32_t p1 = MEM32(a[3]);
        for (unsigned i = 0; i < s_owned_n; ++i)
            if (s_owned[i].handle == a[0] && !s_owned[i].base && p1) {
                s_owned[i].base = p1 - a[1];
                dsh_set_buffer_data(a[0], s_owned[i].base, s_owned[i].bytes);
            }
    } else if (!strcmp(n, "IDirectSoundBuffer_SetLoopRegion")) {
        dsh_set_loop_region(a[0], a[1], a[2]);
    } else if (!strcmp(n, "IDirectSoundBuffer_SetCurrentPosition")) {
        dsh_set_current_position(a[0], a[1]);
    } else if (!strcmp(n, "IDirectSoundBuffer_Play")) {
        dsh_play(a[0], a[3]);
    } else if (!strcmp(n, "IDirectSoundBuffer_Stop")) {
        dsh_stop(a[0]);
    } else if (!strcmp(n, "IDirectSoundBuffer_SetFrequency")) {
        dsh_set_frequency(a[0], a[1]);
    } else if (!strcmp(n, "IDirectSoundBuffer_SetVolume")) {
        dsh_set_volume(a[0], (int32_t)a[1]);
    } else if (!strcmp(n, "IDirectSoundBuffer_SetHeadroom")) {
        dsh_set_headroom(a[0], a[1]);
    } else if (!strcmp(n, "IDirectSoundBuffer_Release")) {
        if (eax == 0) {
            dsh_buffer_release(a[0]);
            for (unsigned i = 0; i < s_owned_n; ++i)
                if (s_owned[i].handle == a[0]) s_owned[i] = s_owned[--s_owned_n];
        }
    } else if (!strcmp(n, "IDirectSoundBuffer_GetStatus")) {
        atomic_fetch_add(&s_status_calls, 1);
        if (!dsh_buffer_exists(a[0])) { atomic_fetch_add(&s_status_unknown_buf, 1); return; }
        uint32_t ds = MEM32(a[1]), mo = dsh_get_status(a[0]);
        if (ds == mo) atomic_fetch_add(&s_status_agree, 1);
        else if ((mo & 1u) && !(ds & 1u)) atomic_fetch_add(&s_status_model_playing_only, 1);
        else if (!(mo & 1u) && (ds & 1u)) atomic_fetch_add(&s_status_dsound_playing_only, 1);
        else atomic_fetch_add(&s_status_other, 1);
        if (ds != mo && s_logged_disagree < 60) {
            s_logged_disagree++;
            fprintf(stderr, "[DSOUND-SHADOW] status disagree buf=%08X dsound=%08X model=%08X\n", a[0], ds, mo);
        }
    } else if (!strcmp(n, "IDirectSoundBuffer_GetCurrentPosition")) {
        atomic_fetch_add(&s_pos_calls, 1);
        if (!dsh_buffer_exists(a[0])) { atomic_fetch_add(&s_pos_unknown_buf, 1); return; }
        uint32_t dp = a[1] ? MEM32(a[1]) : 0, mp = 0;
        dsh_get_current_position(a[0], &mp, NULL);
        uint32_t bpms = fmt_bpms(a[0]);
        uint32_t diff = dp > mp ? dp - mp : mp - dp;
        uint32_t ms = bpms ? (uint32_t)((uint64_t)diff * 100u / bpms) : 0xFFFFFFFFu;
        static const uint32_t edge[7] = { 1, 5, 10, 20, 50, 100, 500 };
        unsigned b = 7;
        for (unsigned i = 0; i < 7; ++i) if (ms < edge[i]) { b = i; break; }
        atomic_fetch_add(&s_pos_hist[b], 1);
    }
    {
        static _Atomic unsigned long next = 20000;
        unsigned long c = atomic_load(&s_calls), due = atomic_load(&next);
        if (c >= due && atomic_compare_exchange_strong(&next, &due, due + 20000)) report("periodic");
    }
}
