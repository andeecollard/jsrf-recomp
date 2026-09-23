/* The host DSOUND. See dsound_host.h and
 * docs/jsrf/plans/JSRF_PLAN_2026-09-23_LIFT_AT_THE_DSOUND_BOUNDARY.md. */
#include "dsound_host.h"

#include <math.h>
#include <stdatomic.h>
#include <string.h>

#define DSH_MAX 2048u               /* open-addressed; the tutorial holds ~390 */
#define ADPCM_FRAMES 65u            /* per block, per channel */
#define FRAC_BITS 32

typedef struct voice {
    uint32_t handle;                /* 0 = free slot, 1 = tombstone */
    dsh_format fmt;
    uint32_t data_va, bytes;
    uint32_t loop_start, loop_len;  /* bytes */
    uint32_t freq;                  /* 0 = fmt.rate */
    int32_t  volume;                /* centibels */
    uint32_t headroom;              /* centibels */
    int      playing, looping;
    uint64_t pos;                   /* frames, 32.32 fixed point */
    float    mat[2][2];             /* [source channel][out L/R] */
    int      is3d;
    uint32_t mode3d;
    float    pos3d[3], min_d, max_d;
    /* one decoded ADPCM block */
    uint32_t cache_block;           /* block index + 1; 0 = empty */
    int16_t  cache[ADPCM_FRAMES * 2];
} voice;

static voice       g_v[DSH_MAX];
static atomic_flag g_lock = ATOMIC_FLAG_INIT;
static dsh_mem_fn  g_mem;
static uint32_t    g_out_rate = 48000;
static dsh_stats   g_st;
static float       g_lpos[3], g_lfront[3] = { 0, 0, 1 }, g_ltop[3] = { 0, 1, 0 };
static float       g_ldist = 1.0f, g_lroll = 1.0f;

static void lock(void)   { while (atomic_flag_test_and_set_explicit(&g_lock, memory_order_acquire)) ; }
static void unlock(void) { atomic_flag_clear_explicit(&g_lock, memory_order_release); }

#define TOMB 1u
static uint32_t hash(uint32_t h) { h ^= h >> 16; h *= 0x7FEB352Du; h ^= h >> 15; return h & (DSH_MAX - 1u); }

static voice *find(uint32_t handle)
{
    if (handle <= TOMB) return NULL;
    for (uint32_t i = 0, k = hash(handle); i < DSH_MAX; ++i, k = (k + 1u) & (DSH_MAX - 1u)) {
        if (g_v[k].handle == handle) return &g_v[k];
        if (g_v[k].handle == 0) return NULL;
    }
    return NULL;
}

static voice *insert(uint32_t handle)
{
    voice *tomb = NULL;
    for (uint32_t i = 0, k = hash(handle); i < DSH_MAX; ++i, k = (k + 1u) & (DSH_MAX - 1u)) {
        if (g_v[k].handle == handle) return &g_v[k];
        if (g_v[k].handle == TOMB && !tomb) tomb = &g_v[k];
        if (g_v[k].handle == 0) return tomb ? tomb : &g_v[k];
    }
    return tomb;
}

void dsh_init(dsh_mem_fn mem, uint32_t out_rate)
{
    lock();
    g_mem = mem;
    g_out_rate = out_rate ? out_rate : 48000u;
    unlock();
}

void dsh_reset(void)
{
    lock();
    memset(g_v, 0, sizeof g_v);
    memset(&g_st, 0, sizeof g_st);
    unlock();
}

/* ---- geometry: bytes <-> frames ---- */

static uint32_t frames_of(const voice *v, uint32_t bytes)
{
    if (v->fmt.tag == DSH_TAG_XBOX_ADPCM)
        return (bytes / v->fmt.block_align) * ADPCM_FRAMES;
    return bytes / v->fmt.block_align;
}

static uint32_t bytes_of(const voice *v, uint32_t frames)
{
    if (v->fmt.tag == DSH_TAG_XBOX_ADPCM)
        return (frames / ADPCM_FRAMES) * v->fmt.block_align;
    return frames * v->fmt.block_align;
}

static uint32_t total_frames(const voice *v) { return frames_of(v, v->bytes); }

static uint32_t loop_start_f(const voice *v) { return frames_of(v, v->loop_start); }
static uint32_t loop_end_f(const voice *v)
{
    uint32_t t = total_frames(v);
    uint32_t e = v->loop_len ? frames_of(v, v->loop_start + v->loop_len) : t;
    return e > t ? t : e;
}

static void default_mat(voice *v)
{
    memset(v->mat, 0, sizeof v->mat);
    if (v->fmt.channels == 1) { v->mat[0][0] = v->mat[0][1] = 1.0f; }
    else { v->mat[0][0] = 1.0f; v->mat[1][1] = 1.0f; }
}

int dsh_buffer_create(uint32_t handle, const dsh_format *fmt, uint32_t data_va, uint32_t bytes)
{
    int ok = fmt && fmt->channels >= 1 && fmt->channels <= 2 && fmt->rate
             && ((fmt->tag == DSH_TAG_PCM && (fmt->bits == 16 || fmt->bits == 8)
                  && fmt->block_align == fmt->channels * (fmt->bits / 8u))
                 || (fmt->tag == DSH_TAG_XBOX_ADPCM && fmt->block_align == 36u * fmt->channels));
    lock();
    if (!ok) { g_st.refused_format++; unlock(); return -1; }
    voice *v = handle > TOMB ? insert(handle) : NULL;
    if (!v) { unlock(); return -1; }
    if (v->handle != handle) g_st.buffers++;
    memset(v, 0, sizeof *v);
    v->handle = handle;
    v->fmt = *fmt;
    v->data_va = data_va;
    v->bytes = bytes;
    v->headroom = DSH_HEADROOM_DEFAULT;
    default_mat(v);
    v->min_d = 1.0f; v->max_d = 1e9f;
    g_st.created++;
    unlock();
    return 0;
}

void dsh_buffer_release(uint32_t handle)
{
    lock();
    voice *v = find(handle);
    if (v) {
        memset(v, 0, sizeof *v);
        v->handle = TOMB;
        g_st.buffers--;
        g_st.released++;
    }
    unlock();
}

int dsh_buffer_exists(uint32_t handle)
{
    lock();
    int r = find(handle) != NULL;
    unlock();
    return r;
}

void dsh_set_buffer_data(uint32_t handle, uint32_t data_va, uint32_t bytes)
{
    lock();
    voice *v = find(handle);
    if (v) {
        v->data_va = data_va;
        v->bytes = bytes;
        v->cache_block = 0;
        if ((v->pos >> FRAC_BITS) >= total_frames(v)) v->pos = 0;
    }
    unlock();
}

void dsh_set_loop_region(uint32_t handle, uint32_t start, uint32_t length)
{
    lock();
    voice *v = find(handle);
    if (v) { v->loop_start = start; v->loop_len = length; }
    unlock();
}

void dsh_set_current_position(uint32_t handle, uint32_t pos)
{
    lock();
    voice *v = find(handle);
    if (v) {
        uint32_t f = frames_of(v, pos);
        v->pos = (uint64_t)(f < total_frames(v) ? f : 0u) << FRAC_BITS;
    }
    unlock();
}

void dsh_play(uint32_t handle, uint32_t flags)
{
    lock();
    voice *v = find(handle);
    if (v) {
        v->playing = 1;
        v->looping = (flags & DSH_PLAY_LOOPING) != 0;
        g_st.plays++;
    }
    unlock();
}

void dsh_stop(uint32_t handle)
{
    lock();
    voice *v = find(handle);
    if (v) { v->playing = 0; g_st.stops++; }
    unlock();
}

void dsh_set_frequency(uint32_t handle, uint32_t hz)
{
    lock();
    voice *v = find(handle);
    if (v) v->freq = hz;
    unlock();
}

void dsh_set_volume(uint32_t handle, int32_t cb)
{
    lock();
    voice *v = find(handle);
    if (v) v->volume = cb > 0 ? 0 : cb;
    unlock();
}

void dsh_set_headroom(uint32_t handle, uint32_t cb)
{
    lock();
    voice *v = find(handle);
    if (v) v->headroom = cb;
    unlock();
}

void dsh_set_mixbins(uint32_t handle, uint32_t n, const uint32_t *bins, const int32_t *vols)
{
    lock();
    voice *v = find(handle);
    if (v) {
        if (!n) default_mat(v);
        else {
            memset(v->mat, 0, sizeof v->mat);
            for (uint32_t i = 0; i < n && i < DSH_MIXBIN_MAX; ++i) {
                int32_t cb = vols[i] > 0 ? 0 : vols[i];
                float g = cb <= -10000 ? 0.0f : powf(10.0f, (float)cb / 2000.0f);
                unsigned c = v->fmt.channels == 1 ? 0u : i % 2u;
                switch (bins[i]) {
                case 0: case 4: v->mat[c][0] += g; break;
                case 1: case 5: v->mat[c][1] += g; break;
                case 2: v->mat[c][0] += 0.7071f * g; v->mat[c][1] += 0.7071f * g; break;
                default: break;
                }
            }
        }
    }
    unlock();
}

void dsh_set_3d(uint32_t handle, int enabled)
{
    lock(); voice *v = find(handle); if (v) v->is3d = enabled != 0; unlock();
}
void dsh_set_3d_position(uint32_t handle, float x, float y, float z)
{
    lock(); voice *v = find(handle); if (v) { v->pos3d[0] = x; v->pos3d[1] = y; v->pos3d[2] = z; } unlock();
}
void dsh_set_3d_distances(uint32_t handle, float min_d, float max_d)
{
    lock();
    voice *v = find(handle);
    if (v) { if (min_d >= 0.0f) v->min_d = min_d; if (max_d >= 0.0f) v->max_d = max_d; }
    unlock();
}
void dsh_set_3d_mode(uint32_t handle, uint32_t mode)
{
    lock(); voice *v = find(handle); if (v) v->mode3d = mode; unlock();
}
void dsh_set_listener_position(float x, float y, float z)
{
    lock(); g_lpos[0] = x; g_lpos[1] = y; g_lpos[2] = z; unlock();
}
void dsh_set_listener_orientation(float fx, float fy, float fz, float tx, float ty, float tz)
{
    lock();
    g_lfront[0] = fx; g_lfront[1] = fy; g_lfront[2] = fz;
    g_ltop[0] = tx; g_ltop[1] = ty; g_ltop[2] = tz;
    unlock();
}
void dsh_set_listener_factors(float distance_factor, float rolloff_factor)
{
    lock();
    if (distance_factor > 0.0f) g_ldist = distance_factor;
    if (rolloff_factor >= 0.0f) g_lroll = rolloff_factor;
    unlock();
}

/* Left and right gains for a 3D voice, from the listener. Lock held. */
static void gains_3d(const voice *v, float *gl, float *gr)
{
    float d[3];
    *gl = *gr = 1.0f;
    if (!v->is3d || v->mode3d == 2u) return;
    for (int i = 0; i < 3; ++i) d[i] = v->mode3d == 1u ? v->pos3d[i] : v->pos3d[i] - g_lpos[i];
    float dist = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]) * g_ldist;
    float mn = v->min_d * g_ldist, mx = v->max_d * g_ldist;
    if (mn <= 0.0f) mn = 1e-3f;
    float dc = dist < mn ? mn : dist > mx ? mx : dist;
    float g = g_lroll == 0.0f ? 1.0f : powf(mn / dc, g_lroll);
    float pan = 0.0f;
    if (dist > 1e-4f) {
        float r[3], rl;
        if (v->mode3d == 1u) { r[0] = 1; r[1] = 0; r[2] = 0; }
        else {
            r[0] = g_ltop[1] * g_lfront[2] - g_ltop[2] * g_lfront[1];
            r[1] = g_ltop[2] * g_lfront[0] - g_ltop[0] * g_lfront[2];
            r[2] = g_ltop[0] * g_lfront[1] - g_ltop[1] * g_lfront[0];
        }
        rl = sqrtf(r[0] * r[0] + r[1] * r[1] + r[2] * r[2]);
        if (rl > 1e-6f) pan = (d[0] * r[0] + d[1] * r[1] + d[2] * r[2]) / (rl * dist / g_ldist);
        if (pan > 1.0f) pan = 1.0f;
        if (pan < -1.0f) pan = -1.0f;
    }
    /* Soft: a source hard to one side keeps 30% on the other ear. */
    *gl = g * (pan > 0.0f ? 1.0f - 0.7f * pan : 1.0f);
    *gr = g * (pan < 0.0f ? 1.0f + 0.7f * pan : 1.0f);
}

uint32_t dsh_get_status(uint32_t handle)
{
    lock();
    voice *v = find(handle);
    uint32_t s = 0;
    if (v && v->playing) s = DSH_STATUS_PLAYING | (v->looping ? DSH_STATUS_LOOPING : 0u);
    unlock();
    return s;
}

void dsh_get_current_position(uint32_t handle, uint32_t *play, uint32_t *write)
{
    uint32_t p = 0, w = 0;
    lock();
    voice *v = find(handle);
    if (v && v->bytes) {
        uint32_t f = (uint32_t)(v->pos >> FRAC_BITS);
        p = bytes_of(v, f);
        if (p >= v->bytes) p = 0;
        /* One mix quantum of source ahead: ~10 ms at the voice's rate, block
         * aligned. The title writes behind this, never between the two. */
        uint32_t rate = v->freq ? v->freq : v->fmt.rate;
        uint32_t lead = bytes_of(v, rate / 100u + (v->fmt.tag == DSH_TAG_XBOX_ADPCM ? ADPCM_FRAMES : 0u));
        w = (p + lead) % v->bytes;
    }
    unlock();
    if (play) *play = p;
    if (write) *write = w;
}

/* ---- decode ---- */

int dsh_adpcm_decode_block(int16_t *out, const uint8_t *in, uint32_t in_bytes, int channels)
{
    static const uint16_t step_table[89] = {
        7,8,9,10,11,12,13,14,16,17,19,21,23,25,28,31,
        34,37,41,45,50,55,60,66,73,80,88,97,107,118,130,143,
        157,173,190,209,230,253,279,307,337,371,408,449,494,544,598,658,
        724,796,876,963,1060,1166,1282,1411,1552,1707,1878,2066,2272,2499,2749,3024,
        3327,3660,4026,4428,4871,5358,5894,6484,7132,7845,8630,9493,10442,11487,12635,13899,
        15289,16818,18500,20350,22385,24623,27086,29794,32767 };
    static const int index_table[8] = { -1, -1, -1, -1, 2, 4, 6, 8 };
    int32_t pcm[2];
    int index[2];

    if (channels < 1 || channels > 2 || in_bytes < 36u * (uint32_t)channels) return 0;
    for (int ch = 0; ch < channels; ++ch) {
        pcm[ch] = (int16_t)(in[0] | (in[1] << 8));
        index[ch] = (int8_t)in[2];
        if (index[ch] < 0) index[ch] = 0;
        if (index[ch] > 88) index[ch] = 88;
        out[ch] = (int16_t)pcm[ch];
        in += 4;
    }
    /* 8 chunks of 4 bytes per channel, alternating: each chunk is 8 samples. */
    for (int chunk = 0; chunk < 8; ++chunk) {
        for (int ch = 0; ch < channels; ++ch) {
            for (int n = 0; n < 8; ++n) {
                int nib = (in[n >> 1] >> ((n & 1) * 4)) & 0xF;
                int step = step_table[index[ch]], d = step >> 3;
                if (nib & 1) d += step >> 2;
                if (nib & 2) d += step >> 1;
                if (nib & 4) d += step;
                if (nib & 8) d = -d;
                pcm[ch] += d;
                if (pcm[ch] > 32767) pcm[ch] = 32767;
                if (pcm[ch] < -32768) pcm[ch] = -32768;
                index[ch] += index_table[nib & 7];
                if (index[ch] < 0) index[ch] = 0;
                if (index[ch] > 88) index[ch] = 88;
                out[(1 + chunk * 8 + n) * channels + ch] = (int16_t)pcm[ch];
            }
            in += 4;
        }
    }
    return (int)ADPCM_FRAMES;
}

/* Frame `f` of voice `v` as (left, right), with the loop applied to indices
 * past the loop end so interpolation across the seam reads the loop start.
 * Returns 0 if the data is not reachable. */
static int frame_at(voice *v, uint32_t f, int32_t *l, int32_t *r)
{
    const int ch = v->fmt.channels;
    if (v->looping) {
        uint32_t ls = loop_start_f(v), le = loop_end_f(v);
        if (le > ls && f >= le) f = ls + (f - le) % (le - ls);
    }
    if (f >= total_frames(v)) { *l = *r = 0; return 1; }
    if (v->fmt.tag == DSH_TAG_XBOX_ADPCM) {
        uint32_t blk = f / ADPCM_FRAMES;
        if (v->cache_block != blk + 1u) {
            const uint8_t *p = g_mem ? g_mem(v->data_va + blk * v->fmt.block_align, v->fmt.block_align) : NULL;
            if (!p || !dsh_adpcm_decode_block(v->cache, p, v->fmt.block_align, ch)) return 0;
            v->cache_block = blk + 1u;
        }
        const int16_t *s = v->cache + (f % ADPCM_FRAMES) * (uint32_t)ch;
        *l = s[0]; *r = s[ch - 1];
        return 1;
    }
    const uint8_t *p = g_mem ? g_mem(v->data_va + f * v->fmt.block_align, v->fmt.block_align) : NULL;
    if (!p) return 0;
    if (v->fmt.bits == 8) {
        *l = ((int32_t)p[0] - 128) << 8; *r = ((int32_t)p[ch - 1] - 128) << 8;
    } else {
        *l = (int16_t)(p[0] | (p[1] << 8));
        *r = (int16_t)(p[(ch - 1) * 2] | (p[(ch - 1) * 2 + 1] << 8));
    }
    return 1;
}

static float gain_of(const voice *v)
{
    int32_t cb = v->volume - (int32_t)v->headroom;
    if (cb <= -10000) return 0.0f;
    return powf(10.0f, (float)cb / 2000.0f);
}

void dsh_mix(int16_t *out, uint32_t frames)
{
    static int32_t acc[2 * 4096];
    while (frames) {
        uint32_t n = frames > 4096u ? 4096u : frames;
        memset(acc, 0, sizeof(int32_t) * 2u * n);
        lock();
        g_st.mixes++;
        g_st.playing = 0;
        for (uint32_t k = 0; k < DSH_MAX; ++k) {
            voice *v = &g_v[k];
            if (v->handle <= TOMB || !v->playing) continue;
            if (!v->bytes || !v->data_va) { g_st.missing_data++; continue; }
            g_st.playing++;
            const float g = gain_of(v);
            float g3l, g3r;
            gains_3d(v, &g3l, &g3r);
            const uint32_t rate = v->freq ? v->freq : v->fmt.rate;
            const uint64_t step = ((uint64_t)rate << FRAC_BITS) / g_out_rate;
            const uint32_t tot = total_frames(v);
            for (uint32_t i = 0; i < n; ++i) {
                uint32_t f = (uint32_t)(v->pos >> FRAC_BITS);
                if (v->looping) {
                    uint32_t ls = loop_start_f(v), le = loop_end_f(v);
                    if (le > ls && f >= le) {
                        v->pos -= (uint64_t)(le - ls) << FRAC_BITS;
                        f = (uint32_t)(v->pos >> FRAC_BITS);
                    }
                } else if (f >= tot) {
                    /* Played out: stopped, cursor back to the start. */
                    v->playing = 0;
                    v->pos = 0;
                    break;
                }
                int32_t l0, r0, l1, r1;
                if (!frame_at(v, f, &l0, &r0) || !frame_at(v, f + 1u, &l1, &r1)) {
                    g_st.missing_data++;
                    v->playing = 0;
                    break;
                }
                float t = (float)(uint32_t)(v->pos & 0xFFFFFFFFu) * (1.0f / 4294967296.0f);
                float c0 = (float)l0 + t * (float)(l1 - l0);
                float c1 = (float)r0 + t * (float)(r1 - r0);
                if (v->fmt.channels == 1) {
                    acc[2 * i]     += (int32_t)(g * g3l * c0 * v->mat[0][0]);
                    acc[2 * i + 1] += (int32_t)(g * g3r * c0 * v->mat[0][1]);
                } else {
                    acc[2 * i]     += (int32_t)(g * g3l * (c0 * v->mat[0][0] + c1 * v->mat[1][0]));
                    acc[2 * i + 1] += (int32_t)(g * g3r * (c0 * v->mat[0][1] + c1 * v->mat[1][1]));
                }
                v->pos += step;
            }
        }
        g_st.frames_mixed += n;
        unlock();
        for (uint32_t i = 0; i < 2u * n; ++i)
            out[i] = (int16_t)(acc[i] > 32767 ? 32767 : acc[i] < -32768 ? -32768 : acc[i]);
        out += 2u * n;
        frames -= n;
    }
}

void dsh_get_stats(dsh_stats *s)
{
    lock();
    *s = g_st;
    unlock();
}
