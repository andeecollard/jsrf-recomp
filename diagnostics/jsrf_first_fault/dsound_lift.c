/* G48 phase 4: JSRF's DSOUND entry points answered by the host.
 *
 * Copied into a scratch gen as recomp_zz_dsound_lift.c by
 * stage_dsound_census.py --lift; RECOMP_DSOUND_LIFT=1 arms it. With it armed,
 * every one of the 55 entry points game code calls (experiments/
 * dsound_boundary/entry_points.json) runs a body here INSTEAD of the title's
 * DSOUND. DirectSoundCreate never initialises the APU, so the title's DSOUND
 * interrupt, DPC and timer never run, and the host model in
 * src/apu/dsound_host.c mixes to the audio device.
 *
 * Each body reads its stdcall arguments from the guest stack, sets eax and
 * pops exactly what the original pops (4 + the ret bytes the census verified
 * on 194,156 calls). ebx/esi/edi/ebp are never touched. With
 * RECOMP_DSOUND_CENSUS=1 as well, the census wrapper checks that ABI on these
 * bodies too.
 *
 * Handles are small blocks of real guest memory, so a stray guest read of one
 * lands on mapped zeros. This file keeps its own bookkeeping in them:
 *   +0 magic, +4 refcount, +8 data VA, +12 owned VA, +16 owned bytes,
 *   +20 data bytes. */
#define RECOMP_GENERATED_CODE
#include "recomp_funcs.h"
#include "dsound_host.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

uint32_t xbox_ContiguousAlloc(uint32_t size, uint32_t alignment);

#define MAGIC_DS  0x5344534Cu   /* "LSDS" */
#define MAGIC_BUF 0x4253444Cu   /* "LDSB" */
#define H_BYTES   64u

static int g_lift = -1;
int dsl_on(void)
{
    if (g_lift < 0) {
        /* DEFAULT ON since 24 Sep 2026 (G54.2): three player sessions of
         * 23 Sep on the lift -- 797 s, 1,005 s and the music check -- with 0
         * faults, 1,340 and 1,857 plays, 0 bad handles or missing data, the
         * cop-fight hang gone and "music seems to work great". Unset or any
         * value but "0" is on; RECOMP_DSOUND_LIFT=0 is the old APU path. */
        const char *v = getenv("RECOMP_DSOUND_LIFT");
        g_lift = !(v && strcmp(v, "0") == 0);
        fprintf(stderr, "[DSOUND-LIFT] RECOMP_DSOUND_LIFT=%s\n", g_lift ? "on" : "off");
    }
    return g_lift;
}

static pthread_mutex_t g_m = PTHREAD_MUTEX_INITIALIZER;
static uint32_t g_ds;                        /* the one IDirectSound */
static uint32_t g_pool, g_pool_left;
static uint32_t g_free_h[4096]; static unsigned g_free_hn;
static struct { uint32_t va, bytes; } g_free_mem[64]; static unsigned g_free_mn;
static _Atomic unsigned long g_calls, g_formatless, g_refused, g_bad_handle, g_image_bad;
static uint32_t g_img_desc, g_img_count, g_img_copy, g_img_size;   /* DownloadEffectsImage */

#define ARG(i) MEM32(esp + 4u + 4u * (uint32_t)(i))
static void ret_(uint32_t pop, uint32_t value) { eax = value; esp += 4u + pop; }

static const uint8_t *mem(uint32_t va, uint32_t len)
{
    if (!va || va + len < va) return NULL;
    return (const uint8_t *)((uintptr_t)va + g_xbox_mem_offset);
}
static void gzero(uint32_t va, uint32_t n) { memset((void *)XBOX_PTR(va), 0, n); }

static uint32_t handle_alloc(uint32_t magic)
{
    uint32_t h;
    pthread_mutex_lock(&g_m);
    if (g_free_hn) h = g_free_h[--g_free_hn];
    else {
        if (!g_pool_left) {
            g_pool = xbox_ContiguousAlloc(65536u, 4096u);
            g_pool_left = g_pool ? 65536u / H_BYTES : 0u;
        }
        if (!g_pool_left) { pthread_mutex_unlock(&g_m); return 0; }
        h = g_pool; g_pool += H_BYTES; g_pool_left--;
    }
    pthread_mutex_unlock(&g_m);
    gzero(h, H_BYTES);
    MEM32(h) = magic;
    MEM32(h + 4u) = 1;
    return h;
}

static int is_buf(uint32_t h) { return h && mem(h, H_BYTES) && MEM32(h) == MAGIC_BUF; }

static uint32_t owned_alloc(uint32_t bytes)
{
    uint32_t va = 0;
    pthread_mutex_lock(&g_m);
    for (unsigned i = 0; i < g_free_mn; ++i)
        if (g_free_mem[i].bytes == bytes) { va = g_free_mem[i].va; g_free_mem[i] = g_free_mem[--g_free_mn]; break; }
    pthread_mutex_unlock(&g_m);
    if (!va) va = xbox_ContiguousAlloc((bytes + 4095u) & ~4095u, 4096u);
    if (va) gzero(va, bytes);
    return va;
}

static void owned_free(uint32_t va, uint32_t bytes)
{
    pthread_mutex_lock(&g_m);
    if (g_free_mn < 64) { g_free_mem[g_free_mn].va = va; g_free_mem[g_free_mn].bytes = bytes; g_free_mn++; }
    pthread_mutex_unlock(&g_m);
}

/* THE STREAM LEAD (G46, intro garble). For a buffer DSOUND owns -- the four
 * CRI stream buffers -- every Lock is measured against the play cursor: how
 * far AHEAD of it the title is writing, in ms. A healthy stream writes well
 * ahead. A writer that has fallen behind writes a region the cursor is
 * already inside ("overlap"), and what plays is half old, half new: garble.
 * Histogram in the periodic report; the first 400 locks
 * logged with their time, so the intro can be told from gameplay. */
static _Atomic unsigned long g_lead_hist[8], g_lead_overlap, g_lead_n;
static double g_t0;
static double now_rel(void)
{
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec * 1e-9 - g_t0;
}
static void lead_note(uint32_t h, uint32_t off, uint32_t n, uint32_t size)
{
    uint32_t play = 0, bps = MEM32(h + 24u);
    if (!bps || !size || !(dsh_get_status(h) & DSH_STATUS_PLAYING)) return;
    dsh_get_current_position(h, &play, NULL);
    uint32_t lead = (off + size - play) % size;
    int overlap = n && lead + n > size;                 /* the region covers the cursor */
    double ms = (double)lead * 1000.0 / bps;
    static const double edge[7] = { 5, 10, 20, 50, 100, 200, 300 };
    unsigned b = 7;
    for (unsigned i = 0; i < 7; ++i) if (ms < edge[i]) { b = i; break; }
    atomic_fetch_add(&g_lead_hist[b], 1);
    if (overlap) atomic_fetch_add(&g_lead_overlap, 1);
    if (atomic_fetch_add(&g_lead_n, 1) < 400)
        fprintf(stderr, "[DSOUND-LEAD] t=%.3f buf=%08X off=%u n=%u play=%u lead=%.1f ms%s\n",
                now_rel(), h, off, n, play, ms, overlap ? "  <-- OVERLAP: writing under the cursor" : "");
}

static void report(const char *why)
{
    dsh_stats s; dsh_get_stats(&s);
    fprintf(stderr, "[DSOUND-LIFT] %s calls=%lu buffers=%lu created=%lu released=%lu plays=%lu stops=%lu"
            " playing=%lu frames_mixed=%lu missing_data=%lu underruns=%lu refused=%lu formatless=%lu bad_handle=%lu"
            " effects=%u image_bad=%lu\n",
            why, (unsigned long)g_calls, s.buffers, s.created, s.released, s.plays, s.stops, s.playing,
            s.frames_mixed, s.missing_data, s.underruns, (unsigned long)g_refused, (unsigned long)g_formatless,
            (unsigned long)g_bad_handle, g_img_count, (unsigned long)g_image_bad);
    fprintf(stderr, "[DSOUND-LEAD] %s stream writes ahead of the cursor, ms: <5:%lu <10:%lu <20:%lu <50:%lu"
            " <100:%lu <200:%lu <300:%lu >=300:%lu | overlapping the cursor: %lu\n", why,
            (unsigned long)g_lead_hist[0], (unsigned long)g_lead_hist[1], (unsigned long)g_lead_hist[2],
            (unsigned long)g_lead_hist[3], (unsigned long)g_lead_hist[4], (unsigned long)g_lead_hist[5],
            (unsigned long)g_lead_hist[6], (unsigned long)g_lead_hist[7], (unsigned long)g_lead_overlap);
    fflush(stderr);
}
static void at_exit(void) { report("exit"); }

static void set_mixbins_from(uint32_t h, uint32_t pmb)
{
    if (!pmb) return;
    uint32_t n = MEM32(pmb), pairs = MEM32(pmb + 4u), bins[DSH_MIXBIN_MAX]; int32_t vols[DSH_MIXBIN_MAX];
    if (n > DSH_MIXBIN_MAX) n = DSH_MIXBIN_MAX;
    for (uint32_t i = 0; i < n; ++i) { bins[i] = MEM32(pairs + 8u * i); vols[i] = (int32_t)MEM32(pairs + 8u * i + 4u); }
    dsh_set_mixbins(h, pairs ? n : 0u, bins, vols);
}

/* ---------------------------------------------------------------- IDirectSound */

/* HRESULT DirectSoundCreate(LPGUID, LPDIRECTSOUND *ppDS, LPUNKNOWN) */
void dsl_DirectSoundCreate(void)
{
    uint32_t pp = ARG(1);
    atomic_fetch_add(&g_calls, 1);
    if (!g_ds) {
        dsh_init(mem, 48000u);
        dsh_reset();
        g_t0 = 0; g_t0 = now_rel();
        int out = dsh_output_start();
        g_ds = handle_alloc(MAGIC_DS);
        atexit(at_exit);
        fprintf(stderr, "[DSOUND-LIFT] DirectSoundCreate: ds=%08X output=%s\n", g_ds,
                out == 1 ? "device" : out == 2 ? "paced (no device)" : "NONE");
    } else {
        MEM32(g_ds + 4u) += 1;
    }
    if (pp) MEM32(pp) = g_ds;
    ret_(12, g_ds ? 0u : 0x8007000Eu);
}

static void ds_addref(void)  { atomic_fetch_add(&g_calls, 1); uint32_t h = ARG(0); uint32_t c = h ? ++MEM32(h + 4u) : 0; ret_(4, c); }
static void ds_release(void) { atomic_fetch_add(&g_calls, 1); uint32_t h = ARG(0); uint32_t c = (h && MEM32(h + 4u)) ? --MEM32(h + 4u) : 0; ret_(4, c); }
void dsl_IDirectSound_AddRef(void)  { ds_addref(); }
void dsl_IDirectSound_Release(void) { ds_release(); }

/* HRESULT IDirectSound_CreateSoundBuffer(pDS, LPCDSBUFFERDESC, LPDIRECTSOUNDBUFFER *, LPUNKNOWN) */
void dsl_IDirectSound_CreateSoundBuffer(void)
{
    uint32_t d = ARG(1), pp = ARG(2);
    atomic_fetch_add(&g_calls, 1);
    uint32_t h = handle_alloc(MAGIC_BUF);
    if (!h) { if (pp) MEM32(pp) = 0; ret_(16, 0x8007000Eu); return; }
    uint32_t bytes = MEM32(d + 8u), wf = MEM32(d + 12u), owned = 0;
    if (bytes) owned = owned_alloc(bytes);
    MEM32(h + 8u) = owned; MEM32(h + 12u) = owned; MEM32(h + 16u) = owned ? bytes : 0u; MEM32(h + 20u) = owned ? bytes : 0u;
    if (!wf) {
        atomic_fetch_add(&g_formatless, 1);
    } else {
        dsh_format f = { MEM16(wf), MEM16(wf + 2u), MEM32(wf + 4u), MEM16(wf + 14u), MEM16(wf + 12u) };
        if (dsh_buffer_create(h, &f, owned, owned ? bytes : 0u) == 0) {
            set_mixbins_from(h, MEM32(d + 16u));
            MEM32(h + 24u) = f.tag == 0x69u ? f.rate * f.block_align / 65u : f.rate * f.block_align;   /* bytes/s */
            if (owned) {
                /* A stream buffer: report its cursor ahead of consumption, as the
                 * Xbox's fetch position is (dsound_host.h). */
                static int lead_ms = -1;
                if (lead_ms < 0) {
                    const char *e = getenv("RECOMP_DSOUND_STREAM_LEAD_MS");
                    /* Default 0. It was 100 while CRI was starved of vblank
                     * wakeups (680 underruns at 0 ms, 12 at 100 on the intro);
                     * with that fixed (f3a9b33) the intro reads 0 underruns at
                     * 0 ms, so the lead would only add 100 ms of latency. Kept
                     * as a switch in case a heavier scene needs cushion. */
                    lead_ms = e && *e ? atoi(e) : 0;
                    if (lead_ms < 0 || lead_ms > 300) lead_ms = 0;
                    fprintf(stderr, "[DSOUND-LIFT] stream cursor lead %d ms\n", lead_ms);
                }
                dsh_set_cursor_lead(h, (uint32_t)((uint64_t)MEM32(h + 24u) * (uint32_t)lead_ms / 1000u));
            }
            dsh_set_3d(h, (MEM32(d + 4u) & 0x10u) != 0);        /* DSBCAPS_CTRL3D */
        } else if (atomic_fetch_add(&g_refused, 1) < 20) {
            fprintf(stderr, "[DSOUND-LIFT] REFUSED format tag=%u ch=%u rate=%u bits=%u align=%u flags=%08X bytes=%u"
                    " -> silent buffer %08X\n", f.tag, f.channels, f.rate, f.bits, f.block_align,
                    MEM32(d + 4u), bytes, h);
        }
    }
    if (pp) MEM32(pp) = h;
    ret_(16, 0);
}

/* HRESULT IDirectSound_GetCaps(pDS, LPDSCAPS): free 2D, free 3D, free SGEs, memory. */
void dsl_IDirectSound_GetCaps(void)
{
    uint32_t p = ARG(1);
    atomic_fetch_add(&g_calls, 1);
    if (p) { MEM32(p) = 200; MEM32(p + 4u) = 64; MEM32(p + 8u) = 2047; MEM32(p + 12u) = 0; }
    ret_(8, 0);
}

/* GetSpeakerConfig(pDS or this, LPDWORD): stereo, as Cxbx answers. */
void dsl_IDirectSound_GetSpeakerConfig(void) { atomic_fetch_add(&g_calls, 1); uint32_t p = ARG(1); if (p) MEM32(p) = 0; ret_(8, 0); }
void dsl_CDirectSound_GetSpeakerConfig(void) { atomic_fetch_add(&g_calls, 1); uint32_t p = ARG(1); if (p) MEM32(p) = 0; ret_(8, 0); }

/* HRESULT DownloadEffectsImage(pDS, LPCVOID pvImage, DWORD size, pImageLoc, LPDSEFFECTIMAGEDESC *ppDesc)
 *
 * The DSP never runs here; what the title needs back is the descriptor, whose
 * per-effect state segments are where Set/GetEffectData read and write. It is
 * found as Cxbx finds it (DirectSound.cpp, reversed from Otogi): the header at
 * 0x800 gives the code and state segment sizes in dwords at +4 and +0xC, the
 * descriptor follows the segments from 0x818, and each 32-byte DSEFFECTMAP's
 * code (+0) and state (+8) pointers are image offsets, rebased here onto a
 * guest copy of the image. */
void dsl_IDirectSound_DownloadEffectsImage(void)
{
    uint32_t img = ARG(1), size = ARG(2), pp = ARG(4);
    atomic_fetch_add(&g_calls, 1);
    uint32_t out = 0;
    if (img && size > 0x818u) {
        uint32_t n1 = MEM32(img + 0x804u), n2 = MEM32(img + 0x80Cu);
        uint64_t off = 0x818ull + 4ull * ((uint64_t)n1 + n2);
        uint32_t count = off + 8u <= size ? MEM32(img + (uint32_t)off) : 0xFFFFFFFFu;
        if (count && count <= 64u && off + 8u + 32ull * count <= size) {
            uint32_t copy = owned_alloc(size);
            uint32_t dsz = 8u + 32u * count, desc = owned_alloc(dsz);
            if (copy && desc) {
                memcpy((void *)XBOX_PTR(copy), mem(img, size), size);
                memcpy((void *)XBOX_PTR(desc), mem(img + (uint32_t)off, dsz), dsz);
                for (uint32_t i = 0; i < count; ++i) {
                    MEM32(desc + 8u + 32u * i) += copy;
                    MEM32(desc + 8u + 32u * i + 8u) += copy;
                }
                g_img_desc = desc; g_img_count = count; g_img_copy = copy; g_img_size = size; out = desc;
                fprintf(stderr, "[DSOUND-LIFT] effects image: %u bytes, %u effects, descriptor %08X\n", size, count, desc);
            }
        }
        if (!out) {
            atomic_fetch_add(&g_image_bad, 1);
            fprintf(stderr, "[DSOUND-LIFT] effects image NOT understood: size=%u n1=%u n2=%u count=%u\n",
                    size, n1, n2, count);
        }
    }
    if (pp) MEM32(pp) = out;
    ret_(20, 0);
}

static uint32_t effect_state(uint32_t idx, uint32_t off, uint32_t n)
{
    if (!g_img_desc || idx >= g_img_count) return 0;
    uint32_t st = MEM32(g_img_desc + 8u + 32u * idx + 8u);
    /* Bounded by the guest copy of the image the state segment lives in. */
    if (st < g_img_copy || (uint64_t)st + off + n > (uint64_t)g_img_copy + g_img_size) return 0;
    return st + off;
}
/* SetEffectData(pDS, idx, offset, pvData, size, apply) / GetEffectData(pDS, idx, offset, pvData, size) */
void dsl_IDirectSound_SetEffectData(void)
{
    uint32_t idx = ARG(1), off = ARG(2), p = ARG(3), n = ARG(4), st = effect_state(idx, off, n);
    atomic_fetch_add(&g_calls, 1);
    if (st && p && n) memcpy((void *)XBOX_PTR(st), mem(p, n), n);
    ret_(24, 0);
}
void dsl_IDirectSound_GetEffectData(void)
{
    uint32_t idx = ARG(1), off = ARG(2), p = ARG(3), n = ARG(4), st = effect_state(idx, off, n);
    atomic_fetch_add(&g_calls, 1);
    if (p && n) { if (st) memcpy((void *)XBOX_PTR(p), mem(st, n), n); else gzero(p, n); }
    ret_(20, 0);
}

#define NOOP(name, pop) void dsl_##name(void) { atomic_fetch_add(&g_calls, 1); ret_(pop, 0); }
static float F(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }

/* The listener (G48 §2c: moved every frame, 181,169 SetPosition calls in a
 * 13-minute replay). Deferred and immediate settings are both applied at
 * once; the title commits every frame anyway. I3DL2 is always "room -10000"
 * (reverb off) in this title, so it is accepted and ignored. */
void dsl_IDirectSound_SetPosition(void)
{
    atomic_fetch_add(&g_calls, 1);
    dsh_set_listener_position(F(ARG(1)), F(ARG(2)), F(ARG(3)));
    ret_(20, 0);
}
void dsl_IDirectSound_SetOrientation(void)
{
    atomic_fetch_add(&g_calls, 1);
    dsh_set_listener_orientation(F(ARG(1)), F(ARG(2)), F(ARG(3)), F(ARG(4)), F(ARG(5)), F(ARG(6)));
    ret_(32, 0);
}
void dsl_IDirectSound_SetDistanceFactor(void) { atomic_fetch_add(&g_calls, 1); dsh_set_listener_factors(F(ARG(1)), -1.0f); ret_(12, 0); }
void dsl_IDirectSound_SetRolloffFactor(void)  { atomic_fetch_add(&g_calls, 1); dsh_set_listener_factors(-1.0f, F(ARG(1))); ret_(12, 0); }
/* DS3DLISTENER: size, position +4, velocity +16, front +28, top +40,
 * distance factor +52, rolloff +56, doppler +60. */
void dsl_IDirectSound_SetAllParameters(void)
{
    uint32_t p = ARG(1);
    atomic_fetch_add(&g_calls, 1);
    if (p) {
        dsh_set_listener_position(F(MEM32(p + 4u)), F(MEM32(p + 8u)), F(MEM32(p + 12u)));
        dsh_set_listener_orientation(F(MEM32(p + 28u)), F(MEM32(p + 32u)), F(MEM32(p + 36u)),
                                     F(MEM32(p + 40u)), F(MEM32(p + 44u)), F(MEM32(p + 48u)));
        dsh_set_listener_factors(F(MEM32(p + 52u)), F(MEM32(p + 56u)));
    }
    ret_(12, 0);
}
NOOP(IDirectSound_SetDopplerFactor, 12)
NOOP(IDirectSound_SetVelocity, 20)
NOOP(IDirectSound_SetI3DL2Listener, 12)
NOOP(IDirectSound_CommitDeferredSettings, 4)
/* sub_0019E4BC / sub_0019E4C1: DSOUND's own `xor eax,eax; ret 12` and `ret 4`. */
NOOP(sub_0019E4BC, 12)
NOOP(sub_0019E4C1, 4)

/* void DirectSoundDoWork(void) -- also the report clock. */
void dsl_DirectSoundDoWork(void)
{
    static double next;
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    double now = t.tv_sec + t.tv_nsec * 1e-9;
    atomic_fetch_add(&g_calls, 1);
    if (now >= next) { if (next) report("periodic"); next = now + 10.0; }
    esp += 4u;
}
/* sub_0019E523: DSOUND's internal work pump, no arguments, no result used. */
void dsl_sub_0019E523(void) { atomic_fetch_add(&g_calls, 1); esp += 4u; }
/* sub_001A24A3: a scalar-deleting destructor (thiscall, one flags argument),
 * reached only through a DSOUND vtable thunk. Nothing of DSOUND's exists to
 * destroy; return `this` as the original does. */
void dsl_sub_001A24A3(void) { atomic_fetch_add(&g_calls, 1); eax = ecx; esp += 8u; }

/* ---------------------------------------------------------------- IDirectSoundBuffer */

#define BUF_OR_BAIL(pop) uint32_t h = ARG(0); atomic_fetch_add(&g_calls, 1); \
    if (!is_buf(h)) { atomic_fetch_add(&g_bad_handle, 1); ret_(pop, 0x80004005u); return; }

void dsl_IDirectSoundBuffer_AddRef(void)  { BUF_OR_BAIL(4); ret_(4, ++MEM32(h + 4u)); }
void dsl_IDirectSoundBuffer_Release(void)
{
    BUF_OR_BAIL(4);
    uint32_t c = MEM32(h + 4u) ? --MEM32(h + 4u) : 0u;
    if (!c) {
        uint32_t owned = MEM32(h + 12u), ob = MEM32(h + 16u);
        dsh_stop(h);
        dsh_buffer_release(h);
        if (owned) owned_free(owned, ob);
        MEM32(h) = 0;
        pthread_mutex_lock(&g_m);
        if (g_free_hn < 4096) g_free_h[g_free_hn++] = h;
        pthread_mutex_unlock(&g_m);
    }
    ret_(4, c);
}

/* SetBufferData(pBuf, pvBufferData, dwBufferBytes) */
void dsl_IDirectSoundBuffer_SetBufferData(void)
{
    BUF_OR_BAIL(12);
    uint32_t p = ARG(1), n = ARG(2);
    MEM32(h + 8u) = p; MEM32(h + 20u) = n;
    dsh_set_buffer_data(h, p, n);
    ret_(12, 0);
}

/* Lock(pBuf, dwOffset, dwBytes, ppv1, pdw1, ppv2, pdw2, dwFlags) */
void dsl_IDirectSoundBuffer_Lock(void)
{
    BUF_OR_BAIL(32);
    uint32_t off = ARG(1), n = ARG(2), pp1 = ARG(3), pn1 = ARG(4), pp2 = ARG(5), pn2 = ARG(6), fl = ARG(7);
    uint32_t base = MEM32(h + 8u), size = MEM32(h + 20u), p1 = 0, n1 = 0, p2 = 0, n2 = 0;
    if (size && base) {
        if (fl & 1u) dsh_get_current_position(h, NULL, &off);         /* DSBLOCK_FROMWRITECURSOR */
        if (fl & 2u) { off = 0; n = size; }                             /* DSBLOCK_ENTIREBUFFER */
        off %= size;
        if (n > size) n = size;
        n1 = n < size - off ? n : size - off;
        p1 = base + off;
        n2 = n - n1;
        p2 = n2 ? base : 0u;
        if (MEM32(h + 12u)) lead_note(h, off, n, size);
    }
    if (pp1) MEM32(pp1) = p1;
    if (pn1) MEM32(pn1) = n1;
    if (pp2) MEM32(pp2) = p2;
    if (pn2) MEM32(pn2) = n2;
    ret_(32, 0);
}
/* Unlock(pBuf, pv1, n1, pv2, n2): for a stream buffer, the end of what was
 * just written is the mark the mixer's underrun count watches. */
void dsl_IDirectSoundBuffer_Unlock(void)
{
    BUF_OR_BAIL(20);
    uint32_t p1 = ARG(1), n1 = ARG(2), p2 = ARG(3), n2 = ARG(4), base = MEM32(h + 8u);
    if (MEM32(h + 12u) && base) {
        if (p2 && n2) dsh_stream_mark(h, p2 - base + n2);
        else if (p1 && n1) dsh_stream_mark(h, p1 - base + n1);
    }
    ret_(20, 0);
}

/* Play(pBuf, dwReserved1, dwReserved2, dwFlags): LOOPING 1, FROMSTART 2. */
void dsl_IDirectSoundBuffer_Play(void)
{
    BUF_OR_BAIL(16);
    uint32_t fl = ARG(3);
    if (fl & 2u) dsh_set_current_position(h, 0);
    dsh_play(h, fl);
    ret_(16, 0);
}
void dsl_IDirectSoundBuffer_Stop(void) { BUF_OR_BAIL(4); dsh_stop(h); ret_(4, 0); }
void dsl_IDirectSoundBuffer_SetLoopRegion(void) { BUF_OR_BAIL(12); dsh_set_loop_region(h, ARG(1), ARG(2)); ret_(12, 0); }
void dsl_IDirectSoundBuffer_SetCurrentPosition(void) { BUF_OR_BAIL(8); dsh_set_current_position(h, ARG(1)); ret_(8, 0); }
void dsl_IDirectSoundBuffer_GetStatus(void)
{
    BUF_OR_BAIL(8);
    uint32_t p = ARG(1);
    if (p) MEM32(p) = dsh_get_status(h);
    ret_(8, 0);
}
void dsl_IDirectSoundBuffer_GetCurrentPosition(void)
{
    BUF_OR_BAIL(12);
    uint32_t pp = ARG(1), pw = ARG(2), play = 0, write = 0;
    dsh_get_current_position(h, &play, &write);
    if (pp) MEM32(pp) = play;
    if (pw) MEM32(pw) = write;
    ret_(12, 0);
}
void dsl_IDirectSoundBuffer_SetFrequency(void) { BUF_OR_BAIL(8); dsh_set_frequency(h, ARG(1)); ret_(8, 0); }
void dsl_IDirectSoundBuffer_SetVolume(void)    { BUF_OR_BAIL(8); dsh_set_volume(h, (int32_t)ARG(1)); ret_(8, 0); }
void dsl_IDirectSoundBuffer_SetHeadroom(void)  { BUF_OR_BAIL(8); dsh_set_headroom(h, ARG(1)); ret_(8, 0); }
void dsl_IDirectSoundBuffer_SetMixBins(void)   { BUF_OR_BAIL(8); set_mixbins_from(h, ARG(1)); ret_(8, 0); }

/* 3D (G48 §2c). DS3DBUFFER: size, position +4, velocity +16, cone angles
 * +28/+32, cone orientation +36, cone outside volume +48, min +52, max +56,
 * mode +60. Cones, velocity and doppler are not modelled. */
void dsl_IDirectSoundBuffer_SetPosition(void) { BUF_OR_BAIL(20); dsh_set_3d_position(h, F(ARG(1)), F(ARG(2)), F(ARG(3))); ret_(20, 0); }
void dsl_IDirectSoundBuffer_SetMinDistance(void) { BUF_OR_BAIL(12); dsh_set_3d_distances(h, F(ARG(1)), -1.0f); ret_(12, 0); }
void dsl_IDirectSoundBuffer_SetMaxDistance(void) { BUF_OR_BAIL(12); dsh_set_3d_distances(h, -1.0f, F(ARG(1))); ret_(12, 0); }
void dsl_IDirectSoundBuffer_SetMode(void) { BUF_OR_BAIL(12); dsh_set_3d_mode(h, ARG(1)); ret_(12, 0); }
void dsl_IDirectSoundBuffer_SetAllParameters(void)
{
    BUF_OR_BAIL(12);
    uint32_t p = ARG(1);
    if (p) {
        dsh_set_3d_position(h, F(MEM32(p + 4u)), F(MEM32(p + 8u)), F(MEM32(p + 12u)));
        dsh_set_3d_distances(h, F(MEM32(p + 52u)), F(MEM32(p + 56u)));
        dsh_set_3d_mode(h, MEM32(p + 60u));
    }
    ret_(12, 0);
}

/* Envelope, LFO, filter, cones, velocity, doppler and per-source I3DL2:
 * accepted, not modelled. A 13-minute replay never called the first three. */
#define BUF_NOOP(name, pop) void dsl_##name(void) { BUF_OR_BAIL(pop); ret_(pop, 0); }
BUF_NOOP(IDirectSoundBuffer_SetLFO, 8)
BUF_NOOP(IDirectSoundBuffer_SetEG, 8)
BUF_NOOP(IDirectSoundBuffer_SetFilter, 8)
BUF_NOOP(IDirectSoundBuffer_SetConeAngles, 16)
BUF_NOOP(IDirectSoundBuffer_SetConeOrientation, 20)
BUF_NOOP(IDirectSoundBuffer_SetConeOutsideVolume, 12)
BUF_NOOP(IDirectSoundBuffer_SetVelocity, 20)
BUF_NOOP(IDirectSoundBuffer_SetDistanceFactor, 12)
BUF_NOOP(IDirectSoundBuffer_SetDopplerFactor, 12)
BUF_NOOP(IDirectSoundBuffer_SetRolloffFactor, 12)
BUF_NOOP(IDirectSoundBuffer_SetI3DL2Source, 12)
