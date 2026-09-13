/*
 * MCPX APU Voice Processor - Standalone extraction from xemu
 *
 * Copyright (c) 2012 espes
 * Copyright (c) 2018-2019 Jannik Vogel
 * Copyright (c) 2019-2025 Matt Borgerson
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 */

#include "apu_state.h"
#include "fpconv.h"
#include <time.h>

/* #define DEBUG_MCPX */

#ifdef DEBUG_MCPX
#define DPRINTF(fmt, ...) fprintf(stderr, fmt, ## __VA_ARGS__)
#else
#define DPRINTF(fmt, ...) do { } while (0)
#endif

/* ============================================================
 * Voice list register table
 * ============================================================ */

static const struct {
    hwaddr top, current, next;
} voice_list_regs[] = {
    { NV_PAPU_TVL2D, NV_PAPU_CVL2D, NV_PAPU_NVL2D }, /* 2D */
    { NV_PAPU_TVL3D, NV_PAPU_CVL3D, NV_PAPU_NVL3D }, /* 3D */
    { NV_PAPU_TVLMP, NV_PAPU_CVLMP, NV_PAPU_NVLMP }, /* MP */
};

/* ============================================================
 * Notify status helper
 * ============================================================ */

static void set_notify_status(MCPXAPUState *d, uint32_t v, int notifier,
                              int status)
{
    hwaddr notify_offset = d->regs[NV_PAPU_FENADDR];
    notify_offset += 16 * (MCPX_HW_NOTIFIER_BASE_OFFSET +
                           v * MCPX_HW_NOTIFIER_COUNT + notifier);
    notify_offset += 15;

    stb_phys(address_space_memory, notify_offset, (uint8_t)status);
    stb_phys(address_space_memory, notify_offset - 1, 1);

    qatomic_or(&d->regs[NV_PAPU_ISTS],
               NV_PAPU_ISTS_FEVINTSTS | NV_PAPU_ISTS_FENINTSTS);
    d->set_irq = true;
}

/* ============================================================
 * Filter helpers
 * ============================================================ */

/* Per-voice resampler state.
 *
 * `phase` is the fractional position between carry[0] and carry[1], and
 * `carry` holds source samples already fetched from the voice but not yet
 * fully consumed. Both have to persist across calls: a 32-sample output frame
 * almost never ends on an integer source sample, and throwing the remainder
 * away at every frame boundary would put a discontinuity at 1500 Hz. */
#define VOICE_RS_CARRY 4
typedef struct {
    float phase;
    float carry[VOICE_RS_CARRY][2];
    int   ncarry;
} VoiceResampleState;
static VoiceResampleState g_voice_rs[MCPX_HW_MAX_VOICES];

static void voice_resample_reset(uint16_t v)
{
    if (v < MCPX_HW_MAX_VOICES) memset(&g_voice_rs[v], 0, sizeof g_voice_rs[v]);
}

static void voice_reset_filters(MCPXAPUState *d, uint16_t v)
{
    assert(v < MCPX_HW_MAX_VOICES);
    memset(&d->vp.filters[v].svf, 0, sizeof(d->vp.filters[v].svf));
    hrtf_filter_clear_history(&d->vp.filters[v].hrtf);
    if (d->vp.filters[v].resampler) {
        src_reset(d->vp.filters[v].resampler);
    }
    voice_resample_reset(v);
}

static bool voice_should_mute(uint16_t v)
{
    bool m = (g_dbg_voice_monitor >= 0) && (v != g_dbg_voice_monitor);
    return m || mcpx_apu_debug_is_muted(v);
}

/* ============================================================
 * Utility functions
 * ============================================================ */

static float clampf(float v, float mn, float mx)
{
    if (v < mn) return mn;
    if (v > mx) return mx;
    return v;
}

static float attenuate(uint16_t vol)
{
    vol &= 0xFFF;
    return (vol == 0xFFF) ? 0.0f : powf(10.0f, vol / (64.0f * -20.0f));
}

/* ============================================================
 * Voice register accessors (read/write voice struct in RAM)
 * ============================================================ */

static uint32_t voice_get_mask(MCPXAPUState *d, uint16_t voice_handle,
                               hwaddr offset, uint32_t mask)
{
    hwaddr voice = d->regs[NV_PAPU_VPVADDR] + voice_handle * NV_PAVS_SIZE;
    return (ldl_le_phys(address_space_memory, voice + offset) & mask) >>
           ctz32(mask);
}

static void voice_set_mask(MCPXAPUState *d, uint16_t voice_handle,
                           hwaddr offset, uint32_t mask, uint32_t val)
{
    hwaddr voice = d->regs[NV_PAPU_VPVADDR] + voice_handle * NV_PAVS_SIZE;
    uint32_t v = ldl_le_phys(address_space_memory, voice + offset) & ~mask;
    stl_le_phys(address_space_memory, voice + offset,
                v | ((val << ctz32(mask)) & mask));
}

/* ============================================================
 * Voice off / lock
 * ============================================================ */

/* Voice lifecycle counters.
 *
 * The guest's DirectSound service routine only runs when FECTL reports a
 * requested trap, and that trap is raised exactly once per voice that retires
 * (voice_off -> SE2FE_IDLE_VOICE). A silent JSRF run shows the trap never
 * being raised, which narrows to either "no voice ever starts" or "voices
 * start and never reach an exhaustion path". Nothing distinguished those,
 * because neither transition was counted.
 *
 * off= alone cannot make that split for this title, because it counts only
 * voice_off, and voice_off is reached only from VOICE_OFF. JSRF retires
 * almost everything with VOICE_RELEASE instead: measured against xemu on the
 * same US title, boot to title screen issues VOICE_RELEASE 72 times against
 * VOICE_OFF 3. So off=0 is what a run reports whether nothing was released or
 * seventy-two voices were, and the two need separate counters to be told
 * apart. release= is the request; off= is the retirement it should eventually
 * produce once the envelope reaches zero. release>0 beside off=0 is a
 * release that never completes -- a distinct fault from never releasing. */
/* A timestamped ring of voice lifecycle events.
 *
 * The counters below say how many voices started and stopped over a whole run.
 * They cannot answer the question actually in front of us, which is what the
 * guest did during ONE two-second window: our audio matches xemu's in steady
 * state and departs from it for about two seconds around a music transition,
 * with 551 single-sample deltas over 12000 where xemu's whole run never exceeds
 * 8462. A total over 160 s cannot see a two-second event.
 *
 * Stamped with g_apu_out_frames, the count of output samples produced, because
 * the WAV capture is written from the same buffer at the same point -- so the
 * stamp is a sample index INTO THE CAPTURE. No clock conversion, and no pair of
 * clocks that might drift apart and have to be argued about afterwards. Divide
 * by 48000 for seconds into the WAV.
 *
 * A ring, not a log: voice traffic is heavy and the interesting window is
 * usually behind you by the time you know it was interesting. 4096 events is a
 * few seconds of the busiest traffic seen so far and costs 64 KB.
 *
 * Opt-in (RECOMP_VOICE_EVENTS) and read-only. */
#define VOICE_EV_MAX 4096
typedef struct { unsigned long long at; uint16_t handle; uint8_t kind; } VoiceEv;
static VoiceEv g_voice_ev[VOICE_EV_MAX];
static unsigned long g_voice_ev_n;          /* total seen; index is % VOICE_EV_MAX */
static const char *const voice_ev_name[] = { "on", "off", "rel" };

static int voice_ev_on(void)
{
    static int on = -1;
    if (on < 0) on = getenv("RECOMP_VOICE_EVENTS") != NULL;
    return on;
}

static hwaddr get_data_ptr(hwaddr sge_base, unsigned int max_sge, uint32_t addr);

/* Everything voice_get_samples will consult, printed at VOICE_ON.
 *
 * Voice 70 -- the one the title uses to cover its music track change -- is
 * processed for the right number of frames, is never starved or short, and
 * every sample it fetches is zero, while voice 68 beside it fetches real audio.
 * So the question is what its descriptor names and whether there is anything
 * there. Printing the fields alone is not enough: a plausible base address
 * pointing at zeroed memory looks exactly like a correct one, so the first
 * dwords AT that address go out too, and that is what separates "we are reading
 * the wrong place" from "the guest has not filled it yet".
 *
 * Stream voices resolve through the SSL page table rather than BA, so which
 * path the voice takes is printed first -- reading BA for a stream voice would
 * be reading a field that means nothing. */
static void voice_desc_dump(MCPXAPUState *d, uint16_t v)
{
    uint32_t fmt, ba, ebo, cbo, lbo;
    int stream, stereo, ssize, csize, loop;
    if (!voice_ev_on() || v >= MCPX_HW_MAX_VOICES) return;
    fmt    = voice_get_mask(d, v, NV_PAVS_VOICE_CFG_FMT, 0xFFFFFFFFu);
    stream = voice_get_mask(d, v, NV_PAVS_VOICE_CFG_FMT,
                            NV_PAVS_VOICE_CFG_FMT_DATA_TYPE) != 0;
    stereo = voice_get_mask(d, v, NV_PAVS_VOICE_CFG_FMT,
                            NV_PAVS_VOICE_CFG_FMT_STEREO) != 0;
    ssize  = voice_get_mask(d, v, NV_PAVS_VOICE_CFG_FMT,
                            NV_PAVS_VOICE_CFG_FMT_SAMPLE_SIZE);
    csize  = voice_get_mask(d, v, NV_PAVS_VOICE_CFG_FMT,
                            NV_PAVS_VOICE_CFG_FMT_CONTAINER_SIZE);
    loop   = voice_get_mask(d, v, NV_PAVS_VOICE_CFG_FMT,
                            NV_PAVS_VOICE_CFG_FMT_LOOP) != 0;
    ba  = voice_get_mask(d, v, NV_PAVS_VOICE_CUR_PSL_START,
                         NV_PAVS_VOICE_CUR_PSL_START_BA);
    ebo = voice_get_mask(d, v, NV_PAVS_VOICE_PAR_NEXT,
                         NV_PAVS_VOICE_PAR_NEXT_EBO);
    cbo = voice_get_mask(d, v, NV_PAVS_VOICE_PAR_OFFSET,
                         NV_PAVS_VOICE_PAR_OFFSET_CBO);
    lbo = voice_get_mask(d, v, NV_PAVS_VOICE_CUR_PSH_SAMPLE,
                         NV_PAVS_VOICE_CUR_PSH_SAMPLE_LBO);
    fprintf(stderr, "  [VOICE-DESC] voice %u %s fmt=%08X stereo=%d ssize=%d "
            "csize=%d loop=%d ba=%08X cbo=%u ebo=%u lbo=%u ssladdr=%08X\n",
            v, stream ? "STREAM" : "buffer", fmt, stereo, ssize, csize, loop,
            ba, cbo, ebo, lbo, d->regs[NV_PAPU_VPSSLADDR]);
    /* The bytes themselves -- through the SAME translation voice_get_samples
     * uses, which is the only reason this line is worth anything.
     *
     * BA is NOT a guest address. For the buffered path it is a linear offset
     * translated through the VPSGEADDR scatter-gather page table
     * (get_data_ptr), and an earlier version of this dump printed guest[ba]
     * directly and produced a tidy, entirely fictional story about voices
     * pointing into low memory and into the game's own code. Translate, or do
     * not print. */
    if (!stream) {
        hwaddr a0 = get_data_ptr(d->regs[NV_PAPU_VPSGEADDR], 0xFFFFFFFF, ba);
        fprintf(stderr, "  [VOICE-DESC]   sge=%08X ba->phys=%08X = "
                "%08X %08X %08X %08X\n",
                d->regs[NV_PAPU_VPSGEADDR], (unsigned)a0,
                (unsigned)ldl_le_phys(address_space_memory, a0),
                (unsigned)ldl_le_phys(address_space_memory, a0 + 4),
                (unsigned)ldl_le_phys(address_space_memory, a0 + 8),
                (unsigned)ldl_le_phys(address_space_memory, a0 + 12));
    } else {
        fprintf(stderr, "  [VOICE-DESC]   stream: resolved per-segment through "
                "the SSL, not BA\n");
    }
    fflush(stderr);
}

static void voice_ev_note(unsigned kind, unsigned handle)
{
    extern unsigned long long g_apu_out_frames;
    VoiceEv *e;
    if (!voice_ev_on()) return;
    e = &g_voice_ev[g_voice_ev_n % VOICE_EV_MAX];
    e->at = g_apu_out_frames;
    e->handle = (uint16_t)handle;
    e->kind = (uint8_t)kind;
    g_voice_ev_n++;
}

/* Events per second of produced audio, so a transition shows up as a spike in
 * a column rather than as something a reader has to spot in 4096 lines. */
void mcpx_apu_voice_events_report(void)
{
    unsigned long start, i;
    unsigned long long sec_lo = 0;
    unsigned on = 0, off = 0, rel = 0;
    char detail[256] = {0};
    int dn = 0;
    if (!voice_ev_on()) return;
    fprintf(stderr, "  [VOICE-EV] %lu events; per second of output audio:\n",
            g_voice_ev_n);
    /* Then the same events at full resolution.
     *
     * The per-second rows below answer "which second did something happen in",
     * which was the right question while finding the window. It is the wrong
     * question now: the ~300 ms silence at the music change sits between an
     * off68 and an on68 that BOTH land in the same second, so the summary
     * cannot say whether the gap is the guest's own pacing between stopping and
     * restarting the voice, or our latency in starting it. The stamp is an
     * output-sample index and always has been; only the report threw the
     * precision away. Milliseconds here are milliseconds into the WAV capture,
     * exactly, because both are counted from the same buffer. */
    {
        unsigned long i2, start2 = g_voice_ev_n > VOICE_EV_MAX
                                 ? g_voice_ev_n - VOICE_EV_MAX : 0;
        for (i2 = start2; i2 < g_voice_ev_n; i2++) {
            const VoiceEv *e2 = &g_voice_ev[i2 % VOICE_EV_MAX];
            fprintf(stderr, "  [VOICE-EV]   %10.3f ms  %-3s voice %u\n",
                    (double)e2->at / 48.0,
                    voice_ev_name[e2->kind < 3 ? e2->kind : 0], e2->handle);
        }
    }
    start = g_voice_ev_n > VOICE_EV_MAX ? g_voice_ev_n - VOICE_EV_MAX : 0;
    for (i = start; i < g_voice_ev_n; i++) {
        const VoiceEv *e = &g_voice_ev[i % VOICE_EV_MAX];
        unsigned long long s = e->at / 48000ull;
        if (s != sec_lo) {
            if (on | off | rel)
                fprintf(stderr, "  [VOICE-EV]   t=%4llus  on=%-3u off=%-3u release=%-3u | %s\n",
                        sec_lo, on, off, rel, detail);
            sec_lo = s; on = off = rel = 0; dn = 0; detail[0] = 0;
        }
        if (e->kind == 0) on++; else if (e->kind == 1) off++; else rel++;
        /* The handles, not just the counts. Two seconds in the last run each
         * had two starts and two stops; one of them corrupted the audio and the
         * other did not, so "how many voices changed" is not the question --
         * "which voice" is, and it is what ties a second to a row of
         * [VOICE-RATE]. */
        if (dn < (int)sizeof detail - 12)
            dn += snprintf(detail + dn, sizeof detail - dn, "%s%s%u",
                           dn ? " " : "", voice_ev_name[e->kind < 3 ? e->kind : 0],
                           e->handle);
    }
    if (on | off | rel)
        fprintf(stderr, "  [VOICE-EV]   t=%4llus  on=%-3u off=%-3u release=%-3u | %s\n",
                sec_lo, on, off, rel, detail);
    fflush(stderr);
}

unsigned long g_apu_voice_on_count;
unsigned long g_apu_voice_off_count;
unsigned long g_apu_voice_release_count;
unsigned long g_apu_idle_trap_count;
unsigned long g_apu_voice_process_count;

/* Front-end method arrivals, counted before the switch decides anything.
 *
 * on=0 answers "did a voice start", but it cannot separate "the guest never
 * wrote VOICE_ON" from "the write never reached this model". The guest's
 * submission loop writes SET_CURRENT_VOICE, VOICE_LOCK and the voice config
 * registers on the same page and in the same iteration as VOICE_ON, so a
 * nonzero fe/current-voice count next to on=0 localises the failure to the
 * loop's own control flow rather than to MMIO routing -- and both being zero
 * says the routing never delivered anything from this loop. */
unsigned long g_apu_fe_method_count;
unsigned long g_apu_set_current_voice_count;
unsigned long g_apu_voice_on_loop_count;

void mcpx_apu_voice_report(void)
{
    fprintf(stderr, "  [APU-VOICE] on=%lu off=%lu release=%lu idle_trap=%lu"
            " processed=%lu"
            " fe_methods=%lu set_current_voice=%lu on_loop=%lu\n",
            g_apu_voice_on_count, g_apu_voice_off_count,
            g_apu_voice_release_count,
            g_apu_idle_trap_count, g_apu_voice_process_count,
            g_apu_fe_method_count, g_apu_set_current_voice_count,
            g_apu_voice_on_loop_count);
    fflush(stderr);
}

static void voice_off(MCPXAPUState *d, uint16_t v)
{
    g_apu_voice_off_count++;
    voice_ev_note(1, v);
    voice_set_mask(d, v, NV_PAVS_VOICE_PAR_STATE,
                   NV_PAVS_VOICE_PAR_STATE_ACTIVE_VOICE, 0);

    bool stream = voice_get_mask(d, v, NV_PAVS_VOICE_CFG_FMT,
                                 NV_PAVS_VOICE_CFG_FMT_DATA_TYPE) != 0;
    int notifier = MCPX_HW_NOTIFIER_SSLA_DONE;
    if (stream) {
        assert(v < MCPX_HW_MAX_VOICES);
        assert(d->vp.ssl[v].ssl_index <= 1);
        notifier += d->vp.ssl[v].ssl_index;
    }
    set_notify_status(d, v, notifier, NV1BA0_NOTIFICATION_STATUS_DONE_SUCCESS);
}

static void voice_lock(MCPXAPUState *d, uint16_t v, bool lock)
{
    assert(v < MCPX_HW_MAX_VOICES);
    /* Reached from a trapped guest store, so the wait has to be announced or
     * the frame thread never lets go of the lock. */
    mcpx_apu_lock_guest(d);

    uint64_t mask = 1ULL << (v % 64);
    if (lock) {
        d->vp.voice_locked[v / 64] |= mask;
    } else {
        d->vp.voice_locked[v / 64] &= ~mask;
    }

    qemu_cond_signal(&d->cond);
    mcpx_apu_unlock_guest(d);
}

static bool is_voice_locked(MCPXAPUState *d, uint16_t v)
{
    assert(v < MCPX_HW_MAX_VOICES);
    uint64_t mask = 1ULL << (v % 64);
    return (qatomic_read(&d->vp.voice_locked[v / 64]) & mask) != 0;
}

/* ============================================================
 * HRIR coefficient setter
 * ============================================================ */

static void set_hrir_coeff_tar(MCPXAPUState *d, int channel, int coeff_idx,
                               int8_t value)
{
    int entry = d->vp.hrtf.current_entry;
    d->vp.hrtf.entries[entry].hrir[channel][coeff_idx] = int8_to_float(value);
}

/* ============================================================
 * Front-End method dispatch
 * ============================================================ */

static void fe_method(MCPXAPUState *d, uint32_t method, uint32_t argument)
{
    unsigned int slot;

    g_apu_fe_method_count++;
    d->regs[NV_PAPU_FEDECMETH] = method;
    d->regs[NV_PAPU_FEDECPARAM] = argument;
    unsigned int selected_handle, list;

    switch (method) {
    case NV1BA0_PIO_VOICE_LOCK:
        voice_lock(d, (uint16_t)d->regs[NV_PAPU_FECV], argument & 1);
        break;

    case NV1BA0_PIO_SET_ANTECEDENT_VOICE:
        d->regs[NV_PAPU_FEAV] = argument;
        break;

    case NV1BA0_PIO_VOICE_ON: {
        g_apu_voice_on_count++;
        selected_handle = argument & NV1BA0_PIO_VOICE_ON_HANDLE;
        voice_ev_note(0, selected_handle);
        voice_desc_dump(d, (uint16_t)selected_handle);
        /* off < on is only a defect for one-shots. A looping voice reaching
         * ebo takes cbo = lbo and runs for ever by design (see voice_process),
         * so BGM never retires and never raises the idle trap. Split the two
         * here, at the only point where a voice starts. */
        if (voice_get_mask(d, (uint16_t)selected_handle, NV_PAVS_VOICE_CFG_FMT,
                           NV_PAVS_VOICE_CFG_FMT_LOOP))
            g_apu_voice_on_loop_count++;

        bool locked = is_voice_locked(d, (uint16_t)selected_handle);
        if (!locked) voice_lock(d, (uint16_t)selected_handle, true);

        list = GET_MASK(d->regs[NV_PAPU_FEAV], NV_PAPU_FEAV_LST);
        if (list != NV1BA0_PIO_SET_ANTECEDENT_VOICE_LIST_INHERIT) {
            unsigned int top_reg = voice_list_regs[list - 1].top;
            voice_set_mask(d, (uint16_t)selected_handle,
                           NV_PAVS_VOICE_TAR_PITCH_LINK,
                           NV_PAVS_VOICE_TAR_PITCH_LINK_NEXT_VOICE_HANDLE,
                           d->regs[top_reg]);
            d->regs[top_reg] = selected_handle;
        } else {
            unsigned int antecedent_voice =
                GET_MASK(d->regs[NV_PAPU_FEAV], NV_PAPU_FEAV_VALUE);
            assert(antecedent_voice != 0xFFFF);

            uint32_t next_handle = voice_get_mask(
                d, (uint16_t)antecedent_voice, NV_PAVS_VOICE_TAR_PITCH_LINK,
                NV_PAVS_VOICE_TAR_PITCH_LINK_NEXT_VOICE_HANDLE);
            voice_set_mask(d, (uint16_t)selected_handle,
                           NV_PAVS_VOICE_TAR_PITCH_LINK,
                           NV_PAVS_VOICE_TAR_PITCH_LINK_NEXT_VOICE_HANDLE,
                           next_handle);
            voice_set_mask(d, (uint16_t)antecedent_voice,
                           NV_PAVS_VOICE_TAR_PITCH_LINK,
                           NV_PAVS_VOICE_TAR_PITCH_LINK_NEXT_VOICE_HANDLE,
                           selected_handle);
        }

        voice_set_mask(d, (uint16_t)selected_handle, NV_PAVS_VOICE_PAR_OFFSET,
                       NV_PAVS_VOICE_PAR_OFFSET_CBO, 0);
        d->vp.ssl[selected_handle].ssl_seg = 0;
        d->vp.ssl[selected_handle].ssl_index = 0;

        unsigned int ea_start = GET_MASK(argument, NV1BA0_PIO_VOICE_ON_ENVA);
        voice_set_mask(d, (uint16_t)selected_handle, NV_PAVS_VOICE_PAR_STATE,
                       NV_PAVS_VOICE_PAR_STATE_EACUR, ea_start);
        if (ea_start == NV_PAVS_VOICE_PAR_STATE_EFCUR_DELAY) {
            uint16_t delay_time =
                (uint16_t)voice_get_mask(d, (uint16_t)selected_handle,
                    NV_PAVS_VOICE_CFG_ENV0, NV_PAVS_VOICE_CFG_ENV0_EA_DELAYTIME);
            voice_set_mask(d, (uint16_t)selected_handle, NV_PAVS_VOICE_CUR_ECNT,
                           NV_PAVS_VOICE_CUR_ECNT_EACOUNT, delay_time * 16);
        } else if (ea_start == NV_PAVS_VOICE_PAR_STATE_EFCUR_ATTACK) {
            voice_set_mask(d, (uint16_t)selected_handle, NV_PAVS_VOICE_CUR_ECNT,
                           NV_PAVS_VOICE_CUR_ECNT_EACOUNT, 0);
        } else if (ea_start == NV_PAVS_VOICE_PAR_STATE_EFCUR_HOLD) {
            uint16_t hold_time =
                (uint16_t)voice_get_mask(d, (uint16_t)selected_handle,
                    NV_PAVS_VOICE_CFG_ENVA, NV_PAVS_VOICE_CFG_ENVA_EA_HOLDTIME);
            voice_set_mask(d, (uint16_t)selected_handle, NV_PAVS_VOICE_CUR_ECNT,
                           NV_PAVS_VOICE_CUR_ECNT_EACOUNT, hold_time * 16);
        }

        unsigned int ef_start = GET_MASK(argument, NV1BA0_PIO_VOICE_ON_ENVF);
        voice_set_mask(d, (uint16_t)selected_handle, NV_PAVS_VOICE_PAR_STATE,
                       NV_PAVS_VOICE_PAR_STATE_EFCUR, ef_start);
        if (ef_start == NV_PAVS_VOICE_PAR_STATE_EFCUR_DELAY) {
            uint16_t delay_time =
                (uint16_t)voice_get_mask(d, (uint16_t)selected_handle,
                    NV_PAVS_VOICE_CFG_ENV1, NV_PAVS_VOICE_CFG_ENV0_EA_DELAYTIME);
            voice_set_mask(d, (uint16_t)selected_handle, NV_PAVS_VOICE_CUR_ECNT,
                           NV_PAVS_VOICE_CUR_ECNT_EFCOUNT, delay_time * 16);
        } else if (ef_start == NV_PAVS_VOICE_PAR_STATE_EFCUR_ATTACK) {
            voice_set_mask(d, (uint16_t)selected_handle, NV_PAVS_VOICE_CUR_ECNT,
                           NV_PAVS_VOICE_CUR_ECNT_EFCOUNT, 0);
        } else if (ef_start == NV_PAVS_VOICE_PAR_STATE_EFCUR_HOLD) {
            uint16_t hold_time =
                (uint16_t)voice_get_mask(d, (uint16_t)selected_handle,
                    NV_PAVS_VOICE_CFG_ENVF, NV_PAVS_VOICE_CFG_ENVA_EA_HOLDTIME);
            voice_set_mask(d, (uint16_t)selected_handle, NV_PAVS_VOICE_CUR_ECNT,
                           NV_PAVS_VOICE_CUR_ECNT_EFCOUNT, hold_time * 16);
        }

        voice_reset_filters(d, (uint16_t)selected_handle);
        voice_set_mask(d, (uint16_t)selected_handle, NV_PAVS_VOICE_PAR_STATE,
                       NV_PAVS_VOICE_PAR_STATE_ACTIVE_VOICE, 1);

        if (!locked) voice_lock(d, (uint16_t)selected_handle, false);
        break;
    }

    case NV1BA0_PIO_VOICE_RELEASE: {
        g_apu_voice_release_count++;
        selected_handle = argument & NV1BA0_PIO_VOICE_ON_HANDLE;
        voice_ev_note(2, selected_handle);

        bool locked = is_voice_locked(d, (uint16_t)selected_handle);
        if (!locked) voice_lock(d, (uint16_t)selected_handle, true);

        uint16_t rr;
        rr = (uint16_t)voice_get_mask(d, (uint16_t)selected_handle,
            NV_PAVS_VOICE_TAR_LFO_ENV, NV_PAVS_VOICE_TAR_LFO_ENV_EA_RELEASERATE);
        voice_set_mask(d, (uint16_t)selected_handle, NV_PAVS_VOICE_CUR_ECNT,
                       NV_PAVS_VOICE_CUR_ECNT_EACOUNT, rr * 16);
        voice_set_mask(d, (uint16_t)selected_handle, NV_PAVS_VOICE_PAR_STATE,
                       NV_PAVS_VOICE_PAR_STATE_EACUR,
                       NV_PAVS_VOICE_PAR_STATE_EFCUR_RELEASE);

        rr = (uint16_t)voice_get_mask(d, (uint16_t)selected_handle,
            NV_PAVS_VOICE_CFG_MISC, NV_PAVS_VOICE_CFG_MISC_EF_RELEASERATE);
        voice_set_mask(d, (uint16_t)selected_handle, NV_PAVS_VOICE_CUR_ECNT,
                       NV_PAVS_VOICE_CUR_ECNT_EFCOUNT, rr * 16);
        voice_set_mask(d, (uint16_t)selected_handle, NV_PAVS_VOICE_PAR_STATE,
                       NV_PAVS_VOICE_PAR_STATE_EFCUR,
                       NV_PAVS_VOICE_PAR_STATE_EFCUR_RELEASE);

        if (!locked) voice_lock(d, (uint16_t)selected_handle, false);
        break;
    }

    case NV1BA0_PIO_VOICE_OFF:
        voice_off(d, (uint16_t)(argument & NV1BA0_PIO_VOICE_OFF_HANDLE));
        break;

    case NV1BA0_PIO_VOICE_PAUSE:
        voice_set_mask(d, (uint16_t)(argument & NV1BA0_PIO_VOICE_PAUSE_HANDLE),
                       NV_PAVS_VOICE_PAR_STATE, NV_PAVS_VOICE_PAR_STATE_PAUSED,
                       (argument & NV1BA0_PIO_VOICE_PAUSE_ACTION) != 0);
        break;

    case NV1BA0_PIO_SET_CURRENT_HRTF_ENTRY:
        d->vp.hrtf.current_entry =
            GET_MASK(argument, NV1BA0_PIO_SET_CURRENT_HRTF_ENTRY_HANDLE);
        break;

    case NV1BA0_PIO_SET_CURRENT_VOICE:
        g_apu_set_current_voice_count++;
        d->regs[NV_PAPU_FECV] = argument;
        break;

    case NV1BA0_PIO_SET_VOICE_CFG_VBIN:
        voice_set_mask(d, (uint16_t)d->regs[NV_PAPU_FECV],
                       NV_PAVS_VOICE_CFG_VBIN, 0xFFFFFFFF, argument);
        break;
    case NV1BA0_PIO_SET_VOICE_CFG_FMT:
        voice_set_mask(d, (uint16_t)d->regs[NV_PAPU_FECV],
                       NV_PAVS_VOICE_CFG_FMT, 0xFFFFFFFF, argument);
        break;
    case NV1BA0_PIO_SET_VOICE_CFG_ENV0:
        voice_set_mask(d, (uint16_t)d->regs[NV_PAPU_FECV],
                       NV_PAVS_VOICE_CFG_ENV0, 0xFFFFFFFF, argument);
        break;
    case NV1BA0_PIO_SET_VOICE_CFG_ENVA:
        voice_set_mask(d, (uint16_t)d->regs[NV_PAPU_FECV],
                       NV_PAVS_VOICE_CFG_ENVA, 0xFFFFFFFF, argument);
        break;
    case NV1BA0_PIO_SET_VOICE_CFG_ENV1:
        voice_set_mask(d, (uint16_t)d->regs[NV_PAPU_FECV],
                       NV_PAVS_VOICE_CFG_ENV1, 0xFFFFFFFF, argument);
        break;
    case NV1BA0_PIO_SET_VOICE_CFG_ENVF:
        voice_set_mask(d, (uint16_t)d->regs[NV_PAPU_FECV],
                       NV_PAVS_VOICE_CFG_ENVF, 0xFFFFFFFF, argument);
        break;
    case NV1BA0_PIO_SET_VOICE_CFG_MISC:
        voice_set_mask(d, (uint16_t)d->regs[NV_PAPU_FECV],
                       NV_PAVS_VOICE_CFG_MISC, 0xFFFFFFFF, argument);
        break;

    case NV1BA0_PIO_SET_VOICE_TAR_HRTF: {
        int handle = GET_MASK(argument, NV1BA0_PIO_SET_VOICE_TAR_HRTF_HANDLE);
        int current_voice = d->regs[NV_PAPU_FECV];
        voice_set_mask(d, (uint16_t)current_voice,
                       NV_PAVS_VOICE_CFG_HRTF_TARGET,
                       NV_PAVS_VOICE_CFG_HRTF_TARGET_HANDLE, handle);
        if (current_voice < MCPX_HW_MAX_3D_VOICES &&
            handle != HRTF_NULL_HANDLE) {
            assert(handle < HRTF_ENTRY_COUNT);
            hrtf_filter_set_target_params(&d->vp.filters[current_voice].hrtf,
                                          d->vp.hrtf.entries[handle].hrir,
                                          d->vp.hrtf.entries[handle].itd);
        }
        break;
    }

    case NV1BA0_PIO_SET_VOICE_TAR_VOLA:
        voice_set_mask(d, (uint16_t)d->regs[NV_PAPU_FECV],
                       NV_PAVS_VOICE_TAR_VOLA, 0xFFFFFFFF, argument);
        break;
    case NV1BA0_PIO_SET_VOICE_TAR_VOLB:
        voice_set_mask(d, (uint16_t)d->regs[NV_PAPU_FECV],
                       NV_PAVS_VOICE_TAR_VOLB, 0xFFFFFFFF, argument);
        break;
    case NV1BA0_PIO_SET_VOICE_TAR_VOLC:
        voice_set_mask(d, (uint16_t)d->regs[NV_PAPU_FECV],
                       NV_PAVS_VOICE_TAR_VOLC, 0xFFFFFFFF, argument);
        break;
    case NV1BA0_PIO_SET_VOICE_LFO_ENV:
        voice_set_mask(d, (uint16_t)d->regs[NV_PAPU_FECV],
                       NV_PAVS_VOICE_TAR_LFO_ENV, 0xFFFFFFFF, argument);
        break;
    case NV1BA0_PIO_SET_VOICE_TAR_FCA:
        voice_set_mask(d, (uint16_t)d->regs[NV_PAPU_FECV],
                       NV_PAVS_VOICE_TAR_FCA, 0xFFFFFFFF, argument);
        break;
    case NV1BA0_PIO_SET_VOICE_TAR_FCB:
        voice_set_mask(d, (uint16_t)d->regs[NV_PAPU_FECV],
                       NV_PAVS_VOICE_TAR_FCB, 0xFFFFFFFF, argument);
        break;
    case NV1BA0_PIO_SET_VOICE_TAR_PITCH:
        voice_set_mask(d, (uint16_t)d->regs[NV_PAPU_FECV],
                       NV_PAVS_VOICE_TAR_PITCH_LINK,
                       NV_PAVS_VOICE_TAR_PITCH_LINK_PITCH,
                       (argument & NV1BA0_PIO_SET_VOICE_TAR_PITCH_STEP) >> 16);
        break;
    case NV1BA0_PIO_SET_VOICE_CFG_BUF_BASE:
        voice_set_mask(d, (uint16_t)d->regs[NV_PAPU_FECV],
                       NV_PAVS_VOICE_CUR_PSL_START,
                       NV_PAVS_VOICE_CUR_PSL_START_BA, argument);
        break;
    case NV1BA0_PIO_SET_VOICE_CFG_BUF_LBO:
        voice_set_mask(d, (uint16_t)d->regs[NV_PAPU_FECV],
                       NV_PAVS_VOICE_CUR_PSH_SAMPLE,
                       NV_PAVS_VOICE_CUR_PSH_SAMPLE_LBO, argument);
        break;
    case NV1BA0_PIO_SET_VOICE_BUF_CBO:
        voice_set_mask(d, (uint16_t)d->regs[NV_PAPU_FECV],
                       NV_PAVS_VOICE_PAR_OFFSET,
                       NV_PAVS_VOICE_PAR_OFFSET_CBO, argument);
        break;
    case NV1BA0_PIO_SET_VOICE_CFG_BUF_EBO:
        voice_set_mask(d, (uint16_t)d->regs[NV_PAPU_FECV],
                       NV_PAVS_VOICE_PAR_NEXT,
                       NV_PAVS_VOICE_PAR_NEXT_EBO, argument);
        break;

    case NV1BA0_PIO_SET_CURRENT_INBUF_SGE:
        d->vp.inbuf_sge_handle = argument & NV1BA0_PIO_SET_CURRENT_INBUF_SGE_HANDLE;
        break;

    case NV1BA0_PIO_SET_CURRENT_INBUF_SGE_OFFSET: {
        hwaddr sge_address =
            d->regs[NV_PAPU_VPSGEADDR] + d->vp.inbuf_sge_handle * 8;
        stl_le_phys(address_space_memory, sge_address,
                    argument & NV1BA0_PIO_SET_CURRENT_INBUF_SGE_OFFSET_PARAMETER);
        break;
    }

    case NV1BA0_PIO_SET_CURRENT_OUTBUF_SGE:
        d->vp.outbuf_sge_handle =
            argument & NV1BA0_PIO_SET_CURRENT_OUTBUF_SGE_HANDLE;
        break;

    case NV1BA0_PIO_SET_CURRENT_OUTBUF_SGE_OFFSET: {
        hwaddr sge_address =
            d->regs[NV_PAPU_VPSGEADDR] + d->vp.outbuf_sge_handle * 8;
        stl_le_phys(address_space_memory, sge_address,
                    argument & NV1BA0_PIO_SET_CURRENT_OUTBUF_SGE_OFFSET_PARAMETER);
        break;
    }

    case NV1BA0_PIO_SET_VOICE_SSL_A: {
        int ssl = 0;
        int current_voice = d->regs[NV_PAPU_FECV];
        assert(current_voice < MCPX_HW_MAX_VOICES);
        d->vp.ssl[current_voice].base[ssl] =
            GET_MASK(argument, NV1BA0_PIO_SET_VOICE_SSL_A_BASE);
        d->vp.ssl[current_voice].count[ssl] =
            (uint8_t)GET_MASK(argument, NV1BA0_PIO_SET_VOICE_SSL_A_COUNT);
        break;
    }
    case NV1BA0_PIO_SET_VOICE_SSL_B: {
        int ssl = 1;
        int current_voice = d->regs[NV_PAPU_FECV];
        assert(current_voice < MCPX_HW_MAX_VOICES);
        d->vp.ssl[current_voice].base[ssl] =
            GET_MASK(argument, NV1BA0_PIO_SET_VOICE_SSL_A_BASE);
        d->vp.ssl[current_voice].count[ssl] =
            (uint8_t)GET_MASK(argument, NV1BA0_PIO_SET_VOICE_SSL_A_COUNT);
        break;
    }

    case NV1BA0_PIO_SET_CURRENT_SSL: {
        assert((argument & 0x3f) == 0);
        assert(argument < (MCPX_HW_MAX_SSL_PRDS * NV_PSGE_SIZE));
        d->vp.ssl_base_page = argument;
        break;
    }

    case NV1BA0_PIO_SET_HRTF_SUBMIXES:
        d->vp.hrtf_submix[0] = (uint8_t)((argument >> 0) & 0x1f);
        d->vp.hrtf_submix[1] = (uint8_t)((argument >> 8) & 0x1f);
        d->vp.hrtf_submix[2] = (uint8_t)((argument >> 16) & 0x1f);
        d->vp.hrtf_submix[3] = (uint8_t)((argument >> 24) & 0x1f);
        break;

    case NV1BA0_PIO_SET_HRTF_HEADROOM:
        d->vp.hrtf_headroom = (uint8_t)(argument & NV1BA0_PIO_SET_HRTF_HEADROOM_AMOUNT);
        break;

    case SE2FE_IDLE_VOICE:
        if (d->regs[NV_PAPU_FETFORCE1] & NV_PAPU_FETFORCE1_SE2FE_IDLE_VOICE) {
            g_apu_idle_trap_count++;
            d->regs[NV_PAPU_FECTL] &= ~NV_PAPU_FECTL_FEMETHMODE;
            d->regs[NV_PAPU_FECTL] |= NV_PAPU_FECTL_FEMETHMODE_TRAPPED;
            d->regs[NV_PAPU_FECTL] &= ~NV_PAPU_FECTL_FETRAPREASON;
            d->regs[NV_PAPU_FECTL] |= NV_PAPU_FECTL_FETRAPREASON_REQUESTED;
            d->set_irq = true;
        }
        break;

    default:
        /* Handle range-based cases that can't use case ranges in MSVC */
        if (method >= NV1BA0_PIO_SET_HRIR && method < NV1BA0_PIO_SET_HRIR_X) {
            assert(d->vp.hrtf.current_entry < HRTF_ENTRY_COUNT);
            slot = (method - NV1BA0_PIO_SET_HRIR) / 4;
            int8_t left0 = (int8_t)GET_MASK(argument, NV1BA0_PIO_SET_HRIR_LEFT0);
            int8_t right0 = (int8_t)GET_MASK(argument, NV1BA0_PIO_SET_HRIR_RIGHT0);
            int8_t left1 = (int8_t)GET_MASK(argument, NV1BA0_PIO_SET_HRIR_LEFT1);
            int8_t right1 = (int8_t)GET_MASK(argument, NV1BA0_PIO_SET_HRIR_RIGHT1);
            int coeff_idx = slot * 2;
            set_hrir_coeff_tar(d, 0, coeff_idx, left0);
            set_hrir_coeff_tar(d, 1, coeff_idx, right0);
            set_hrir_coeff_tar(d, 0, coeff_idx + 1, left1);
            set_hrir_coeff_tar(d, 1, coeff_idx + 1, right1);
        } else if (method == NV1BA0_PIO_SET_HRIR_X) {
            assert(d->vp.hrtf.current_entry < HRTF_ENTRY_COUNT);
            int8_t left30 = (int8_t)GET_MASK(argument, NV1BA0_PIO_SET_HRIR_X_LEFT30);
            int8_t right30 = (int8_t)GET_MASK(argument, NV1BA0_PIO_SET_HRIR_X_RIGHT30);
            int16_t itd = (int16_t)GET_MASK(argument, NV1BA0_PIO_SET_HRIR_X_ITD);
            set_hrir_coeff_tar(d, 0, 30, left30);
            set_hrir_coeff_tar(d, 1, 30, right30);
            d->vp.hrtf.entries[d->vp.hrtf.current_entry].itd = s6p9_to_float(itd);
        } else if (method >= NV1BA0_PIO_SET_SSL_SEGMENT_OFFSET &&
                   method < NV1BA0_PIO_SET_SSL_SEGMENT_LENGTH + 8 * 64) {
            assert((method & 0x3) == 0);
            hwaddr addr = d->regs[NV_PAPU_VPSSLADDR]
                          + (d->vp.ssl_base_page * 8)
                          + (method - NV1BA0_PIO_SET_SSL_SEGMENT_OFFSET);
            stl_le_phys(address_space_memory, addr, argument);
        } else if (method >= NV1BA0_PIO_SET_SUBMIX_HEADROOM &&
                   method <= NV1BA0_PIO_SET_SUBMIX_HEADROOM + 4 * (NUM_MIXBINS - 1)) {
            assert((method & 3) == 0);
            slot = (method - NV1BA0_PIO_SET_SUBMIX_HEADROOM) / 4;
            d->vp.submix_headroom[slot] =
                (uint8_t)(argument & NV1BA0_PIO_SET_SUBMIX_HEADROOM_AMOUNT);
        } else if ((method >= NV1BA0_PIO_SET_OUTBUF_BA &&
                    method < NV1BA0_PIO_SET_OUTBUF_BA + 32) ||
                   (method >= NV1BA0_PIO_SET_OUTBUF_LEN &&
                    method < NV1BA0_PIO_SET_OUTBUF_LEN + 32)) {
            /* Outbuf base/length - ignore for now */
        } else {
            /* Unknown method - silently ignore */
            DPRINTF("Unknown FE method: 0x%08X arg=0x%08X\n", method, argument);
        }
        break;
    }
}

/* ============================================================
 * VP MMIO read/write (exposed to apu_core.c)
 * ============================================================ */

uint64_t mcpx_apu_vp_read(void *opaque, hwaddr addr, unsigned int size)
{
    (void)opaque; (void)size;

    switch (addr) {
    case NV1BA0_PIO_FREE:
        /* Free space in the front end's method FIFO. There is no FIFO here --
         * mcpx_apu_vp_write dispatches each method as it arrives -- so it is
         * always completely empty, and the honest answer is the whole of it.
         *
         * The size matters, because callers do not test this for equality.
         * JSRF's submission path waits twice: once for (FREE & ~3) >= 0x80,
         * and once for (FREE >> 2) >= voices * 7, where the voice count is a
         * byte. 0x80 satisfies the first wait but only the second for four
         * voices or fewer, and a fifth would spin here for ever. Reporting the
         * full window satisfies both for any count a byte can hold. */
        return 0xFFFC;
    default:
        break;
    }
    return 0;
}

void mcpx_apu_vp_write(void *opaque, hwaddr addr, uint64_t val,
                        unsigned int size)
{
    MCPXAPUState *d = (MCPXAPUState *)opaque;
    (void)size;

    /* Dispatch known methods through fe_method */
    fe_method(d, (uint32_t)addr, (uint32_t)val);
}

/* ============================================================
 * SGE data pointer resolution
 * ============================================================ */

static hwaddr get_data_ptr(hwaddr sge_base, unsigned int max_sge, uint32_t addr)
{
    unsigned int entry = addr / TARGET_PAGE_SIZE;
    assert(entry <= max_sge);
    uint32_t prd_address =
        ldl_le_phys(address_space_memory, sge_base + entry * 4 * 2);
    return prd_address + addr % TARGET_PAGE_SIZE;
}

/* ============================================================
 * Envelope processing
 * ============================================================ */

static float voice_step_envelope(MCPXAPUState *d, uint16_t v, uint32_t reg_0,
                           uint32_t reg_a, uint32_t rr_reg, uint32_t rr_mask,
                           uint32_t lvl_reg, uint32_t lvl_mask,
                           uint32_t count_mask, uint32_t cur_mask)
{
    uint8_t cur = (uint8_t)voice_get_mask(d, v, NV_PAVS_VOICE_PAR_STATE, cur_mask);
    switch (cur) {
    case NV_PAVS_VOICE_PAR_STATE_EFCUR_OFF:
        voice_set_mask(d, v, NV_PAVS_VOICE_CUR_ECNT, count_mask, 0);
        voice_set_mask(d, v, lvl_reg, lvl_mask, 0xFF);
        return 1.0f;

    case NV_PAVS_VOICE_PAR_STATE_EFCUR_DELAY: {
        uint16_t count =
            (uint16_t)voice_get_mask(d, v, NV_PAVS_VOICE_CUR_ECNT, count_mask);
        voice_set_mask(d, v, lvl_reg, lvl_mask, 0x00);
        if (count == 0) {
            cur++;
            voice_set_mask(d, v, NV_PAVS_VOICE_PAR_STATE, cur_mask, cur);
        } else {
            count--;
        }
        voice_set_mask(d, v, NV_PAVS_VOICE_CUR_ECNT, count_mask, count);
        return 0.0f;
    }

    case NV_PAVS_VOICE_PAR_STATE_EFCUR_ATTACK: {
        uint16_t count =
            (uint16_t)voice_get_mask(d, v, NV_PAVS_VOICE_CUR_ECNT, count_mask);
        uint16_t attack_rate =
            (uint16_t)voice_get_mask(d, v, reg_0, NV_PAVS_VOICE_CFG_ENV0_EA_ATTACKRATE);
        float value;
        if (attack_rate == 0) {
            value = 255.0f;
        } else {
            if (count <= (uint32_t)(attack_rate * 16)) {
                value = (count * 0xFF) / (float)(attack_rate * 16);
            } else {
                value = 255.0f;
            }
        }
        voice_set_mask(d, v, lvl_reg, lvl_mask, (uint32_t)value);
        if (count == (uint32_t)(attack_rate * 16)) {
            cur++;
            voice_set_mask(d, v, NV_PAVS_VOICE_PAR_STATE, cur_mask, cur);
            uint16_t hold_time =
                (uint16_t)voice_get_mask(d, v, reg_a, NV_PAVS_VOICE_CFG_ENVA_EA_HOLDTIME);
            count = hold_time * 16;
        } else {
            count++;
        }
        voice_set_mask(d, v, NV_PAVS_VOICE_CUR_ECNT, count_mask, count);
        return value / 255.0f;
    }

    case NV_PAVS_VOICE_PAR_STATE_EFCUR_HOLD: {
        uint16_t count =
            (uint16_t)voice_get_mask(d, v, NV_PAVS_VOICE_CUR_ECNT, count_mask);
        voice_set_mask(d, v, lvl_reg, lvl_mask, 0xFF);
        if (count == 0) {
            cur++;
            voice_set_mask(d, v, NV_PAVS_VOICE_PAR_STATE, cur_mask, cur);
            uint16_t decay_rate =
                (uint16_t)voice_get_mask(d, v, reg_a, NV_PAVS_VOICE_CFG_ENVA_EA_DECAYRATE);
            count = decay_rate * 16;
        } else {
            count--;
        }
        voice_set_mask(d, v, NV_PAVS_VOICE_CUR_ECNT, count_mask, count);
        return 1.0f;
    }

    case NV_PAVS_VOICE_PAR_STATE_EFCUR_DECAY: {
        uint16_t count =
            (uint16_t)voice_get_mask(d, v, NV_PAVS_VOICE_CUR_ECNT, count_mask);
        uint16_t decay_rate =
            (uint16_t)voice_get_mask(d, v, reg_a, NV_PAVS_VOICE_CFG_ENVA_EA_DECAYRATE);
        uint8_t sustain_level =
            (uint8_t)voice_get_mask(d, v, reg_a, NV_PAVS_VOICE_CFG_ENVA_EA_SUSTAINLEVEL);
        float value;
        if (decay_rate == 0) {
            value = 0.0f;
        } else {
            value = 255.0f * powf(0.99988799f, (decay_rate * 16 - count) *
                                                   4096.0f / decay_rate);
        }
        if (value <= (sustain_level + 0.2f) || (value > 255.0f)) {
            cur++;
            voice_set_mask(d, v, NV_PAVS_VOICE_PAR_STATE, cur_mask, cur);
        } else {
            count--;
            voice_set_mask(d, v, NV_PAVS_VOICE_CUR_ECNT, count_mask, count);
            voice_set_mask(d, v, lvl_reg, lvl_mask, (uint32_t)value);
        }
        return value / 255.0f;
    }

    case NV_PAVS_VOICE_PAR_STATE_EFCUR_SUSTAIN: {
        uint8_t sustain_level =
            (uint8_t)voice_get_mask(d, v, reg_a, NV_PAVS_VOICE_CFG_ENVA_EA_SUSTAINLEVEL);
        voice_set_mask(d, v, NV_PAVS_VOICE_CUR_ECNT, count_mask, 0x00);
        voice_set_mask(d, v, lvl_reg, lvl_mask, sustain_level);
        return sustain_level / 255.0f;
    }

    case NV_PAVS_VOICE_PAR_STATE_EFCUR_RELEASE: {
        uint16_t count =
            (uint16_t)voice_get_mask(d, v, NV_PAVS_VOICE_CUR_ECNT, count_mask);
        uint16_t release_rate = (uint16_t)voice_get_mask(d, v, rr_reg, rr_mask);
        if (release_rate == 0) count = 0;
        float value = 0;
        if (count == 0) {
            voice_set_mask(d, v, NV_PAVS_VOICE_PAR_STATE, cur_mask, ++cur);
        } else {
            float pos = clampf(1 - count / (release_rate * 16.0f), 0, 1);
            uint8_t lvl = (uint8_t)voice_get_mask(d, v, lvl_reg, lvl_mask);
            value = powf((float)M_E, -6.91f * pos) * lvl;
            count--;
            voice_set_mask(d, v, NV_PAVS_VOICE_CUR_ECNT, count_mask, count);
        }
        return value / 255.0f;
    }

    case NV_PAVS_VOICE_PAR_STATE_EFCUR_FORCE_RELEASE:
        if (count_mask == NV_PAVS_VOICE_CUR_ECNT_EACOUNT) {
            voice_off(d, v);
        }
        return 0.0f;

    default:
        fprintf(stderr, "[APU] Unknown envelope state 0x%x\n", cur);
        return 0.0f;
    }
}

/* ============================================================
 * Sample fetching from voice buffers
 * ============================================================ */

/* What playback rate each voice actually asks for.
 *
 * voice_resample() used to take a rate and discard it, playing one source
 * sample per output sample whatever the pitch register said. It resamples now;
 * this stayed because it is what says WHICH voices depend on that, and it is
 * the check that catches a voice whose rate we still handle badly. rate is
 * 1/2^(pitch/4096), so rate == 1.0 exactly when the pitch register is zero.
 *
 * Whether that MATTERS is an empirical question and this is how to settle it
 * rather than argue it. If every voice the title starts asks for 1.0, the
 * missing resampler costs nothing and is not the audio defect. If a voice asks
 * for anything else, it is being played at the wrong speed, and a streaming
 * voice played at the wrong speed drains its buffer at the wrong speed -- which
 * is a mechanism that produces exactly what we see: fine in steady state,
 * wrong at the moment a stream is switched or refilled.
 *
 * Per voice rather than in aggregate, because "some voice somewhere had a
 * non-unit rate" cannot be acted on. Counted in frames, with min and max, so a
 * voice that is briefly bent (a pitch envelope) is distinguishable from one
 * playing at a flat wrong rate for its whole life.
 *
 * Opt-in (RECOMP_VOICE_RATES), read-only, one float compare per voice frame. */
typedef struct {
    unsigned long loopbacks, starved_loops;
    unsigned long fresh_slots, stale_slots, new_slots;
    unsigned long cur_stale_run, max_stale_run;
    /* Same two counts, per report window, reset by the report. Cumulative
     * totals averaged the silent pre-music part of the intro together with the
     * part that stutters and hid a five-fold difference; [FRAME-WIN] exists for
     * the same reason, and these are printed so the two can be read off against
     * each other in the same window. */
    unsigned long w_fresh_slots, w_stale_slots;
    /* Leading edges: a stale slot followed by a fresh one is the start of one
     * chunk the guest wrote. Counting edges rather than slots turns the deficit
     * into a CADENCE in Hz, which can be matched against the clocks that exist
     * -- vblank at 59.7, the APU frame at 1500, a 1 ms sleep at 1000 -- instead
     * of being described as a percentage that fits nothing. */
    unsigned long w_refill_edges;
    int last_slot_stale;
    unsigned long frames, off_frames;
    unsigned long short_calls, dry_calls, short_samples;
    unsigned long silent_frames;      /* fetched samples were all ~zero */
    double energy;                    /* sum |sample| of what the voice produced */
    float min_rate, max_rate, last_rate;
} VoiceRate;
static VoiceRate g_voice_rate[MCPX_HW_MAX_VOICES];

static int voice_fresh_on(void);

/* RECOMP_VOICE_FRESH implies this: the freshness counters are reported by
 * mcpx_apu_voice_rate_report, and the per-voice loop there skips any voice with
 * no counted frames, so arming freshness alone would print nothing at all. */
static int voice_rate_on(void)
{
    static int on = -1;
    if (on < 0)
        on = getenv("RECOMP_VOICE_RATES") != NULL || voice_fresh_on();
    return on;
}

static void voice_rate_note(uint16_t v, float rate)
{
    VoiceRate *r;
    if (!voice_rate_on() || v >= MCPX_HW_MAX_VOICES) return;
    r = &g_voice_rate[v];
    if (!r->frames) { r->min_rate = rate; r->max_rate = rate; }
    if (rate < r->min_rate) r->min_rate = rate;
    if (rate > r->max_rate) r->max_rate = rate;
    r->last_rate = rate;
    r->frames++;
    /* 1e-6 rather than ==: rate comes out of powf, and a pitch of exactly 0
     * should give exactly 1.0f but nothing here depends on that being bit
     * exact. Anything this close plays back indistinguishably. */
    if (rate < 1.0f - 1e-6f || rate > 1.0f + 1e-6f) r->off_frames++;
}


/* Is the guest still WRITING the buffer we are reading out of?
 *
 * Measured against xemu, our intro plays the same music sample-for-sample
 * (r=1.00) for about two seconds and then starts losing ground -- 85 ms of
 * forward progress lost in a 200 ms window, with disruptions too close together
 * for a 100 ms window to correlate at all. Content right, continuity wrong.
 * That is the signature of a streaming ring buffer whose producer has fallen
 * behind its consumer: the play cursor laps the write cursor and re-plays the
 * previous pass, and every sample of that is valid music, which is why five
 * different characterisations of the output called it clean.
 *
 * Inferring that from the audio is not enough -- so count it in the mechanism.
 * The buffer is divided into slots of one APU frame; each slot's contents are
 * hashed as we read them and compared with what the SAME slot held on the
 * previous pass. The guest refilling normally makes every slot differ: music
 * does not repeat bit-exactly. A guest that has fallen behind makes them
 * identical, and max_stale_run says how much unbroken stale audio was played.
 *
 * The prediction from the oracle is specific, which is the point of measuring
 * rather than arguing: stale ~0 for the first two seconds of music, then a
 * quarter to a half of slots stale.
 *
 * Opt-in (RECOMP_VOICE_FRESH), read-only, one multiply-xor per sample. */
#define VOICE_FRESH_SLOTS 2048u

typedef struct {
    uint32_t crc[VOICE_FRESH_SLOTS];
    uint8_t  seen[VOICE_FRESH_SLOTS];
    /* Per slot, how many passes over the ring found it unchanged. This is what
     * separates the two explanations for a constant stale fraction, and they
     * want opposite fixes: a producer that cannot keep up leaves DIFFERENT
     * slots stale on every lap, so every slot is stale some of the time; a
     * buffer we are reading through the wrong mapping leaves the SAME slots
     * stale for ever, so the histogram is bimodal and the always-stale ones are
     * contiguous. */
    uint16_t stale_n[VOICE_FRESH_SLOTS], pass_n[VOICE_FRESH_SLOTS];
    uint32_t acc, slot;
    int      started;
} VoiceFresh;
static VoiceFresh g_voice_fresh[MCPX_HW_MAX_VOICES];

static int voice_fresh_on(void)
{
    static int on = -1;
    if (on < 0) on = getenv("RECOMP_VOICE_FRESH") != NULL;
    return on;
}

static void voice_fresh_commit(uint16_t v, VoiceFresh *f)
{
    VoiceRate *r = &g_voice_rate[v];
    uint32_t sl = f->slot;
    if (!f->seen[sl]) {
        f->seen[sl] = 1;
        r->new_slots++;
        r->cur_stale_run = 0;
    } else if (f->crc[sl] == f->acc) {
        r->stale_slots++;
        r->w_stale_slots++;
        r->last_slot_stale = 1;
        if (f->stale_n[sl] < 0xFFFF) f->stale_n[sl]++;
        if (++r->cur_stale_run > r->max_stale_run)
            r->max_stale_run = r->cur_stale_run;
    } else {
        r->fresh_slots++;
        r->w_fresh_slots++;
        if (r->last_slot_stale) r->w_refill_edges++;
        r->last_slot_stale = 0;
        r->cur_stale_run = 0;
    }
    if (f->pass_n[sl] < 0xFFFF) f->pass_n[sl]++;
    f->crc[sl] = f->acc;
}

static void voice_fresh_sample(uint16_t v, uint32_t cbo, const float s[2])
{
    VoiceFresh *f;
    uint32_t slot;
    union { float f; uint32_t u; } c0, c1;
    if (!voice_fresh_on() || v >= MCPX_HW_MAX_VOICES) return;
    f = &g_voice_fresh[v];
    slot = (cbo / NUM_SAMPLES_PER_FRAME) % VOICE_FRESH_SLOTS;
    if (!f->started) {
        f->started = 1; f->slot = slot; f->acc = 2166136261u;
    } else if (slot != f->slot) {
        voice_fresh_commit(v, f);
        f->slot = slot; f->acc = 2166136261u;
    }
    c0.f = s[0]; c1.f = s[1];
    f->acc = (f->acc ^ c0.u) * 16777619u;
    f->acc = (f->acc ^ c1.u) * 16777619u;
}

static int voice_get_samples(MCPXAPUState *d, uint32_t v, float samples[][2],
                       int num_samples_requested)
{
    assert(v < MCPX_HW_MAX_VOICES);
    bool stereo = voice_get_mask(d, (uint16_t)v, NV_PAVS_VOICE_CFG_FMT,
                                 NV_PAVS_VOICE_CFG_FMT_STEREO) != 0;
    unsigned int channels = stereo ? 2 : 1;
    unsigned int sample_size = voice_get_mask(
        d, (uint16_t)v, NV_PAVS_VOICE_CFG_FMT, NV_PAVS_VOICE_CFG_FMT_SAMPLE_SIZE);
    unsigned int container_sizes[4] = { 1, 2, 0, 4 };
    unsigned int container_size_index = voice_get_mask(
        d, (uint16_t)v, NV_PAVS_VOICE_CFG_FMT, NV_PAVS_VOICE_CFG_FMT_CONTAINER_SIZE);
    unsigned int container_size = container_sizes[container_size_index];
    bool stream = voice_get_mask(d, (uint16_t)v, NV_PAVS_VOICE_CFG_FMT,
                                 NV_PAVS_VOICE_CFG_FMT_DATA_TYPE) != 0;
    bool paused = voice_get_mask(d, (uint16_t)v, NV_PAVS_VOICE_PAR_STATE,
                                 NV_PAVS_VOICE_PAR_STATE_PAUSED) != 0;
    bool loop = voice_get_mask(d, (uint16_t)v, NV_PAVS_VOICE_CFG_FMT,
                               NV_PAVS_VOICE_CFG_FMT_LOOP) != 0;
    uint32_t ebo = voice_get_mask(d, (uint16_t)v, NV_PAVS_VOICE_PAR_NEXT,
                                  NV_PAVS_VOICE_PAR_NEXT_EBO);
    uint32_t cbo = voice_get_mask(d, (uint16_t)v, NV_PAVS_VOICE_PAR_OFFSET,
                                  NV_PAVS_VOICE_PAR_OFFSET_CBO);
    uint32_t lbo = voice_get_mask(d, (uint16_t)v, NV_PAVS_VOICE_CUR_PSH_SAMPLE,
                                  NV_PAVS_VOICE_CUR_PSH_SAMPLE_LBO);
    uint32_t ba = voice_get_mask(d, (uint16_t)v, NV_PAVS_VOICE_CUR_PSL_START,
                                 NV_PAVS_VOICE_CUR_PSL_START_BA);
    unsigned int samples_per_block =
        1 + voice_get_mask(d, (uint16_t)v, NV_PAVS_VOICE_CFG_FMT,
                           NV_PAVS_VOICE_CFG_FMT_SAMPLES_PER_BLOCK);
    bool persist = voice_get_mask(d, (uint16_t)v, NV_PAVS_VOICE_CFG_FMT,
                                  NV_PAVS_VOICE_CFG_FMT_PERSIST) != 0;

    int ssl_index = 0, ssl_seg = 0, page = 0, count = 0;
    int seg_len = 0, seg_cs = 0, seg_spb = 0, seg_s = 0;
    hwaddr segment_offset = 0;
    uint32_t segment_length = 0;
    size_t block_size;

    int adpcm_block_index = -1;
    uint32_t adpcm_block[36 * 2 / 4];
    int16_t adpcm_decoded[65 * 2];

    voice_set_mask(d, (uint16_t)v, NV_PAVS_VOICE_PAR_STATE,
                   NV_PAVS_VOICE_PAR_STATE_NEW_VOICE, 0);

    if (paused) return -1;

    if (stream) {
        if (!persist) {
            int eacur = voice_get_mask(d, (uint16_t)v, NV_PAVS_VOICE_PAR_STATE,
                                       NV_PAVS_VOICE_PAR_STATE_EACUR);
            if (eacur < NV_PAVS_VOICE_PAR_STATE_EFCUR_RELEASE) {
                voice_off(d, (uint16_t)v);
                return -1;
            }
        }

        assert(!loop);
        ssl_index = d->vp.ssl[v].ssl_index;
        ssl_seg = d->vp.ssl[v].ssl_seg;
        page = d->vp.ssl[v].base[ssl_index] + ssl_seg;
        count = d->vp.ssl[v].count[ssl_index];

        if (count == 0) {
            voice_set_mask(d, (uint16_t)v, NV_PAVS_VOICE_PAR_OFFSET,
                           NV_PAVS_VOICE_PAR_OFFSET_CBO, 0);
            d->vp.ssl[v].ssl_seg = 0;
            if (!persist) {
                d->vp.ssl[v].ssl_index = 0;
                voice_off(d, (uint16_t)v);
            } else {
                set_notify_status(d, v, MCPX_HW_NOTIFIER_SSLA_DONE +
                                  d->vp.ssl[v].ssl_index,
                                  NV1BA0_NOTIFICATION_STATUS_DONE_SUCCESS);
            }
            return -1;
        }

        hwaddr addr = d->regs[NV_PAPU_VPSSLADDR] + page * 8;
        segment_offset = ldl_le_phys(address_space_memory, addr);
        segment_length = ldl_le_phys(address_space_memory, addr + 4);
        assert(segment_offset != 0);
        assert(segment_length != 0);
        seg_len = (segment_length >> 0) & 0xffff;
        seg_cs = (segment_length >> 16) & 3;
        seg_spb = (segment_length >> 18) & 0x1f;
        seg_s = (segment_length >> 23) & 1;
        container_size_index = seg_cs;
        if (seg_cs == NV_PAVS_VOICE_CFG_FMT_CONTAINER_SIZE_ADPCM) {
            sample_size = NV_PAVS_VOICE_CFG_FMT_SAMPLE_SIZE_S24;
        }
        assert(seg_len > 0);
        ebo = seg_len - 1;
    }

    bool adpcm =
        (container_size_index == NV_PAVS_VOICE_CFG_FMT_CONTAINER_SIZE_ADPCM);

    if (adpcm) {
        block_size = 36;
    } else {
        block_size = container_size;
    }
    block_size *= samples_per_block;

    int sample_count = 0;
    for (; (sample_count < num_samples_requested) && (cbo <= ebo);
         sample_count++, cbo++) {
        if (adpcm) {
            unsigned int block_index = cbo / ADPCM_SAMPLES_PER_BLOCK;
            unsigned int block_position = cbo % ADPCM_SAMPLES_PER_BLOCK;
            if (adpcm_block_index != (int)block_index) {
                uint32_t linear_addr = block_index * (uint32_t)block_size;
                if (stream) {
                    hwaddr addr = segment_offset + linear_addr;
                    memcpy(adpcm_block, &d->ram_ptr[addr & g_apu_ram_mask],
                           block_size);
                } else {
                    linear_addr += ba;
                    for (unsigned int word_index = 0;
                         word_index < (9 * samples_per_block); word_index++) {
                        hwaddr addr = get_data_ptr(d->regs[NV_PAPU_VPSGEADDR],
                                                   0xFFFFFFFF, linear_addr);
                        adpcm_block[word_index] =
                            ldl_le_phys(address_space_memory, addr);
                        linear_addr += 4;
                    }
                }
                adpcm_decode_block(adpcm_decoded, (uint8_t *)adpcm_block,
                                   block_size, channels);
                adpcm_block_index = block_index;
            }

            samples[sample_count][0] =
                int16_to_float(adpcm_decoded[block_position * channels]);
            if (stereo) {
                samples[sample_count][1] = int16_to_float(
                    adpcm_decoded[block_position * channels + 1]);
            }
        } else {
            hwaddr addr;
            if (stream) {
                addr = segment_offset + cbo * block_size;
            } else {
                uint32_t linear_addr = ba + cbo * (uint32_t)block_size;
                addr = get_data_ptr(d->regs[NV_PAPU_VPSGEADDR], 0xFFFFFFFF,
                                    linear_addr);
            }

            for (unsigned int channel = 0; channel < channels; channel++) {
                uint32_t ival;
                float fval;
                switch (sample_size) {
                case NV_PAVS_VOICE_CFG_FMT_SAMPLE_SIZE_U8:
                    ival = ldub_phys(address_space_memory, addr);
                    fval = uint8_to_float((uint8_t)(ival & 0xff));
                    break;
                case NV_PAVS_VOICE_CFG_FMT_SAMPLE_SIZE_S16:
                    ival = lduw_le_phys(address_space_memory, addr);
                    fval = int16_to_float((int16_t)(ival & 0xffff));
                    break;
                case NV_PAVS_VOICE_CFG_FMT_SAMPLE_SIZE_S24:
                    ival = ldl_le_phys(address_space_memory, addr);
                    fval = int24_to_float(ival);
                    break;
                case NV_PAVS_VOICE_CFG_FMT_SAMPLE_SIZE_S32:
                    ival = ldl_le_phys(address_space_memory, addr);
                    fval = int32_to_float(ival);
                    break;
                default:
                    fval = 0.0f;
                    break;
                }
                samples[sample_count][channel] = fval;
                addr += container_size;
            }
        }

        if (!stereo) {
            samples[sample_count][1] = samples[sample_count][0];
        }

        voice_fresh_sample((uint16_t)v, cbo, samples[sample_count]);
    }

    /* How often the cursor ran past the end of the buffer.
     *
     * For a looping buffered voice this snaps cbo back to lbo, which is correct
     * at a genuine wrap and catastrophic if it happens constantly: the voice
     * then replays whatever short span lies between lbo and a stale ebo, over
     * and over, and every sample of it is valid music. That is audible as a
     * stutter and INVISIBLE to sample statistics -- deltas, gaps and repetition
     * counts on the mixed output all read normal, which is how four separate
     * metrics called a glitchy capture clean.
     *
     * `starved_loops` is the damning subset: the loop-back happened with the
     * play loop having produced NOTHING this call, i.e. cbo was already past
     * ebo on entry. A healthy stream wraps having just played a bufferful; a
     * broken one wraps having played nothing. */
    if (cbo >= ebo) {
        if (voice_rate_on() && v < MCPX_HW_MAX_VOICES) {
            g_voice_rate[v].loopbacks++;
            if (sample_count == 0) g_voice_rate[v].starved_loops++;
        }
        if (stream) {
            d->vp.ssl[v].ssl_seg += 1;
            cbo = 0;
            if (d->vp.ssl[v].ssl_seg < d->vp.ssl[v].count[ssl_index]) {
                /* Move to next segment */
            } else {
                int next_index = (ssl_index + 1) % 2;
                d->vp.ssl[v].ssl_index = next_index;
                d->vp.ssl[v].ssl_seg = 0;
                set_notify_status(d, v, MCPX_HW_NOTIFIER_SSLA_DONE + ssl_index,
                                  NV1BA0_NOTIFICATION_STATUS_DONE_SUCCESS);
            }
        } else {
            if (loop) {
                cbo = lbo;
            } else {
                cbo = ebo;
                voice_off(d, (uint16_t)v);
            }
        }
    }

    voice_set_mask(d, (uint16_t)v, NV_PAVS_VOICE_PAR_OFFSET,
                   NV_PAVS_VOICE_PAR_OFFSET_CBO, cbo);
    return sample_count;
}

/* ============================================================
 * Voice resampling (simplified - no libsamplerate)
 *
 * Since libsamplerate is stubbed, we do a simple nearest-neighbor
 * resample. This gives us functional audio at the cost of quality.
 * ============================================================ */

/* A voice that could not fill the frame it was asked for. The caller breaks out
 * of its fill loop on a short return and leaves the remainder of the frame
 * buffer as it found it, so a short call is a hole in the output -- which is
 * exactly the defect class the delta metric is blind to, because a hole that
 * opens and closes quietly produces no large sample-to-sample step.
 *
 * `dry` is the subset where nothing at all came back: the voice was active and
 * had no data, which is the guest failing to refill a stream rather than
 * anything the resampler does. Separating the two is the point -- one is ours
 * and one is upstream of us. */
static void voice_short_note(uint16_t v, int produced, int requested)
{
    VoiceRate *r;
    if (!voice_rate_on() || v >= MCPX_HW_MAX_VOICES) return;
    if (produced >= requested) return;
    r = &g_voice_rate[v];
    r->short_calls++;
    r->short_samples += (unsigned long)(requested - produced);
    if (produced == 0) r->dry_calls++;
}

/* A starved voice contributes silence, and says so here rather than relying on
 * a memset in its caller.
 *
 * CORRECTION, and it is the point of this comment. An earlier version of this
 * said voice_process mixed "stack residue" into the output on a short return,
 * because `float samples[NUM_SAMPLES_PER_FRAME][2]` is a plain local and the
 * mix loop walks all 32 entries regardless of how many were produced. The
 * second half is true. The first half is not: the very next line after that
 * declaration is `memset(samples, 0, sizeof(samples))`, and git blame puts it
 * in the original import (7a71ed2), so a short return has ALWAYS produced
 * silence in the tail, here and in upstream and in burnout3-research alike.
 * The claim came from reading the declaration and not the line under it.
 *
 * So this is not a bug fix and must not be cited as one. What it is: the
 * guarantee now lives in the function that knows how many samples it produced,
 * instead of depending on a memset in a different function that nothing
 * connects to it. That is where xemu puts it --
 *
 *     if (sample_count < NUM_SAMPLES_PER_FRAME) {
 *         // Starvation causes SRC hang on repeated calls. Provide silence.
 *         memset(&filter->resample_buf[2*sample_count], 0, ...);
 *         sample_count = NUM_SAMPLES_PER_FRAME;
 *     }
 *
 * (hw/xbox/mcpx/apu/vp/vp.c, voice_resample_callback), and xemu's caller is
 * byte-identical to ours, so the placement is the whole of the difference.
 *
 * MEASURED: with RECOMP_VOICE_RATES counting short and dry returns per voice,
 * every voice reports short=0 dry=0 over 154 s reaching the Corn tutorial. This
 * path does not execute. It is cheap insurance, not a repair. */
static void voice_fill_silence(float samples[][2], int from, int to)
{
    if (from < to)
        memset(&samples[from], 0, (size_t)(to - from) * sizeof samples[0]);
}

/* Resample a voice to the output rate.
 *
 * This used to take `rate` and discard it -- one source sample per output
 * sample regardless -- and the comment said "nearest-neighbor", which it was
 * not; it was no resampling at all. MEASURED 13 Sep 2026 with
 * RECOMP_VOICE_RATES: of the voices JSRF starts, 64-67 ask for rate 1.0 and
 * were unaffected, voice 68 asks for 1.0885 (44100 Hz material) for 100% of its
 * frames, and voice 69 asks for 2.1770 (22050 Hz) for 100% of its. Voice 69 is
 * alive only during the three seconds where our captured audio departs from
 * xemu's -- 535 single-sample deltas over 12000 in those seconds and none
 * anywhere else in the run, against an xemu capture whose whole run never
 * exceeds 8462.
 *
 * Two things go wrong when the rate is ignored, and they are worth separating.
 * The obvious one is pitch: 22050 Hz material played at 48000 comes out 2.177x
 * too fast, which multiplies every frequency in it by 2.177 and pushes its top
 * end into the region where consecutive samples can swing the full scale --
 * that is what the deltas are. The less obvious one matters more for a stream:
 * the voice's own cursor advances one sample per output sample, so a streaming
 * buffer is consumed 2.177x faster than the guest is refilling it.
 *
 * Linear interpolation, not nearest-neighbour: nearest-neighbour at a
 * non-integer ratio is a jitter of up to half a sample on every output, which
 * is a broadband noise floor. Linear is not audiophile -- it is a gentle
 * low-pass with some aliasing left -- but it is the difference between wrong
 * and roughly right, and it costs two multiplies.
 *
 * `rate` is output samples per source sample (48000/source_hz), so we advance
 * the source by 1/rate per output sample.
 *
 * RECOMP_NO_RESAMPLE=1 restores the old behaviour so the change can be A/B-ed
 * against itself rather than against a memory of it. */
static int voice_resample_disabled(void)
{
    static int off = -1;
    if (off < 0) off = getenv("RECOMP_NO_RESAMPLE") != NULL;
    return off;
}

static int voice_resample(MCPXAPUState *d, uint16_t v, float samples[][2],
                          int requested_num, float rate)
{
    VoiceResampleState *rs;
    float step;
    int produced = 0;

    /* Unity is the common case -- four of JSRF's six voices -- and it must stay
     * bit-identical to what it was, so it does not go through the interpolator
     * at all. Also the fallback for a nonsense rate: refusing to divide by it
     * is better than producing silence or a NaN that reaches the mixer. */
    if (voice_resample_disabled() || !(rate > 0.0f) || !isfinite(rate)
            || (rate > 0.99999f && rate < 1.00001f)) {
        int sample_count = 0;
        while (sample_count < requested_num) {
            int active = voice_get_mask(d, v, NV_PAVS_VOICE_PAR_STATE,
                                        NV_PAVS_VOICE_PAR_STATE_ACTIVE_VOICE);
            int count;
            if (!active) break;
            count = voice_get_samples(d, v, &samples[sample_count],
                                      requested_num - sample_count);
            if (count < 0) break;
            if (count == 0) return -1;
            sample_count += count;
        }
        voice_short_note(v, sample_count, requested_num);
        voice_fill_silence(samples, sample_count, requested_num);
        return requested_num;
    }

    rs = &g_voice_rs[v];
    step = 1.0f / rate;                    /* source samples per output sample */

    while (produced < requested_num) {
        /* Two source samples are needed to interpolate between. Fetch one at a
         * time: asking for a block would advance the voice's cursor past what
         * this frame actually consumes, which is the same over-consumption
         * being fixed here. */
        while (rs->ncarry < 2) {
            int count;
            if (!voice_get_mask(d, v, NV_PAVS_VOICE_PAR_STATE,
                                NV_PAVS_VOICE_PAR_STATE_ACTIVE_VOICE)) {
                voice_short_note(v, produced, requested_num);
                voice_fill_silence(samples, produced, requested_num);
                return requested_num;
            }
            count = voice_get_samples(d, v, &rs->carry[rs->ncarry], 1);
            if (count <= 0) {
                voice_short_note(v, produced, requested_num);
                voice_fill_silence(samples, produced, requested_num);
                return requested_num;
            }
            rs->ncarry += count;
        }

        samples[produced][0] = rs->carry[0][0] * (1.0f - rs->phase)
                             + rs->carry[1][0] * rs->phase;
        samples[produced][1] = rs->carry[0][1] * (1.0f - rs->phase)
                             + rs->carry[1][1] * rs->phase;
        produced++;

        rs->phase += step;
        while (rs->phase >= 1.0f && rs->ncarry > 0) {
            rs->phase -= 1.0f;
            memmove(&rs->carry[0], &rs->carry[1],
                    (size_t)(rs->ncarry - 1) * sizeof rs->carry[0]);
            rs->ncarry--;
        }
    }
    voice_short_note(v, produced, requested_num);
    return produced;
}

/* Seconds since the previous call. The windowed counters are counts, and a
 * count is not a rate until it is divided by the interval it was collected
 * over -- and REPORT_MS is a request, not a guarantee, so this measures the
 * interval rather than assuming it. */
static double voice_fresh_window_s(void)
{
    static struct timespec prev;
    struct timespec now;
    double dt;
    clock_gettime(CLOCK_MONOTONIC, &now);
    if (!prev.tv_sec && !prev.tv_nsec) { prev = now; return 1.0; }
    dt = (double)(now.tv_sec - prev.tv_sec)
       + (double)(now.tv_nsec - prev.tv_nsec) / 1e9;
    prev = now;
    return dt > 1e-3 ? dt : 1.0;
}

static double g_voice_fresh_window_s = 1.0;

void mcpx_apu_voice_rate_report(void)
{
    unsigned v, shown = 0, any = 0;
    unsigned long tot = 0, tot_off = 0;
    if (!voice_rate_on()) return;
    g_voice_fresh_window_s = voice_fresh_window_s();
    for (v = 0; v < MCPX_HW_MAX_VOICES; v++) {
        tot += g_voice_rate[v].frames;
        tot_off += g_voice_rate[v].off_frames;
        if (g_voice_rate[v].off_frames) any++;
    }
    fprintf(stderr, "  [VOICE-RATE] %lu voice-frames, %lu needing resampling "
            "(%.2f%%), %u voice(s) affected%s\n",
            tot, tot_off, tot ? 100.0 * (double)tot_off / (double)tot : 0.0, any,
            tot ? "" : "   <- NO VOICE FRAMES: nothing is being processed");
    for (v = 0; v < MCPX_HW_MAX_VOICES && shown < 12; v++) {
        const VoiceRate *r = &g_voice_rate[v];
        if (!r->frames) continue;
        shown++;
        /* A rate of R means we play R source samples per output sample; we
         * currently always play 1. So the source is consumed at 1/R times the
         * speed it should be, and 48000/R is the rate the guest asked for. */
        fprintf(stderr, "  [VOICE-RATE]   voice %3u: %lu/%lu frames resampled, "
                "rate min=%.4f max=%.4f last=%.4f  (source ~%.0f Hz -> 48000)\n",
                v, r->off_frames, r->frames, r->min_rate, r->max_rate,
                r->last_rate, r->last_rate > 0.0f ? 48000.0f / r->last_rate : 0.0f);
        fprintf(stderr, "  [VOICE-RATE]        short=%lu (dry=%lu) losing %lu "
                "samples = %.1f ms | energy=%.1f silent_frames=%lu/%lu%s\n",
                r->short_calls, r->dry_calls, r->short_samples,
                r->short_samples / 48.0, r->energy, r->silent_frames, r->frames,
                (r->frames && r->silent_frames == r->frames)
                    ? "   <- PRODUCED NOTHING" : "");
        fprintf(stderr, "  [VOICE-RATE]        loopbacks=%lu of which %lu "
                "played NOTHING first%s\n", r->loopbacks, r->starved_loops,
                (r->starved_loops > r->frames / 8)
                    ? "   <- STUTTERING: wrapping on an empty buffer" : "");
        if (voice_fresh_on()) {
            unsigned long classified = r->fresh_slots + r->stale_slots;
            fprintf(stderr, "  [VOICE-FRESH]       slots: %lu fresh, %lu STALE "
                    "(%.1f%%), %lu first-pass | longest stale run %lu slots "
                    "= %.0f ms%s\n",
                    r->fresh_slots, r->stale_slots,
                    classified ? 100.0 * (double)r->stale_slots
                               / (double)classified : 0.0,
                    r->new_slots, r->max_stale_run,
                    r->max_stale_run * NUM_SAMPLES_PER_FRAME / 48.0,
                    (classified && r->stale_slots > classified / 20)
                        ? "   <- the guest is not refilling this buffer" : "");
            {
                const VoiceFresh *f = &g_voice_fresh[v];
                unsigned b[5] = { 0, 0, 0, 0, 0 }, i, lo = 0, hi = 0, run = 0,
                         best_run = 0, best_lo = 0;
                for (i = 0; i < VOICE_FRESH_SLOTS; i++) {
                    unsigned p = f->pass_n[i], st = f->stale_n[i];
                    if (!p) continue;
                    if (!st) b[0]++;
                    else if (st == p) b[4]++;
                    else if (st * 4 < p) b[1]++;
                    else if (st * 4 > p * 3) b[3]++;
                    else b[2]++;
                    if (st * 4 > p * 3) {
                        if (!run) lo = i;
                        run++;
                        if (run > best_run) { best_run = run; best_lo = lo; }
                    } else run = 0;
                    hi = i;
                }
                fprintf(stderr, "  [VOICE-FRESH]       per-slot over %u slots: "
                        "%u never stale, %u <25%%, %u 25-75%%, %u >75%%, "
                        "%u ALWAYS | longest always-ish span %u slots at %u\n",
                        hi + 1, b[0], b[1], b[2], b[3], b[4], best_run, best_lo);
            }
            {
                VoiceRate *rw = &g_voice_rate[v];
                unsigned long wc = rw->w_fresh_slots + rw->w_stale_slots;
                double dt = g_voice_fresh_window_s;
                fprintf(stderr, "  [VOICE-FRESH-WIN]   this window: %lu fresh, "
                        "%lu stale (%.1f%%) | guest wrote %.0f samples/s of "
                        "%.0f consumed (%.0f%%) in %.1f chunks/s of %.1f ms\n",
                        rw->w_fresh_slots, rw->w_stale_slots,
                        wc ? 100.0 * (double)rw->w_stale_slots / (double)wc : 0.0,
                        (double)rw->w_fresh_slots * NUM_SAMPLES_PER_FRAME / dt,
                        (double)wc * NUM_SAMPLES_PER_FRAME / dt,
                        wc ? 100.0 * (double)rw->w_fresh_slots / (double)wc : 0.0,
                        (double)rw->w_refill_edges / dt,
                        rw->w_refill_edges
                            ? (double)rw->w_fresh_slots / (double)rw->w_refill_edges
                              * NUM_SAMPLES_PER_FRAME / 44.1
                            : 0.0);
                rw->w_fresh_slots = 0; rw->w_stale_slots = 0;
                rw->w_refill_edges = 0;
            }
        }
    }
    fflush(stderr);
}

/* ============================================================
 * Voice processing (main per-voice function)
 * ============================================================ */

static void voice_process(MCPXAPUState *d,
                          float mixbins[NUM_MIXBINS][NUM_SAMPLES_PER_FRAME],
                          float sample_buf[NUM_SAMPLES_PER_FRAME][2],
                          uint16_t v, int voice_list)
{
    assert(v < MCPX_HW_MAX_VOICES);
    bool stereo = voice_get_mask(d, v, NV_PAVS_VOICE_CFG_FMT,
                                 NV_PAVS_VOICE_CFG_FMT_STEREO) != 0;
    unsigned int channels = stereo ? 2 : 1;
    bool paused = voice_get_mask(d, v, NV_PAVS_VOICE_PAR_STATE,
                                 NV_PAVS_VOICE_PAR_STATE_PAUSED) != 0;

    struct McpxApuDebugVoice *dbg = &g_dbg.vp.v[v];
    dbg->active = true;
    dbg->stereo = stereo;
    dbg->paused = paused;

    if (paused) return;

    /* Step filter envelope */
    float ef_value = voice_step_envelope(
        d, v, NV_PAVS_VOICE_CFG_ENV1, NV_PAVS_VOICE_CFG_ENVF,
        NV_PAVS_VOICE_CFG_MISC, NV_PAVS_VOICE_CFG_MISC_EF_RELEASERATE,
        NV_PAVS_VOICE_PAR_NEXT, NV_PAVS_VOICE_PAR_NEXT_EFLVL,
        NV_PAVS_VOICE_CUR_ECNT_EFCOUNT, NV_PAVS_VOICE_PAR_STATE_EFCUR);
    if (ef_value < 0.0f) ef_value = 0.0f;
    if (ef_value > 1.0f) ef_value = 1.0f;

    int16_t p = (int16_t)voice_get_mask(d, v, NV_PAVS_VOICE_TAR_PITCH_LINK,
                                         NV_PAVS_VOICE_TAR_PITCH_LINK_PITCH);
    int8_t ps = (int8_t)voice_get_mask(d, v, NV_PAVS_VOICE_CFG_ENV0,
                                        NV_PAVS_VOICE_CFG_ENV0_EF_PITCHSCALE);
    float rate = 1.0f / powf(2.0f, (p + ps * 32 * ef_value) / 4096.0f);
    dbg->rate = rate;
    voice_rate_note(v, rate);

    /* Step amplitude envelope */
    float ea_value = voice_step_envelope(
        d, v, NV_PAVS_VOICE_CFG_ENV0, NV_PAVS_VOICE_CFG_ENVA,
        NV_PAVS_VOICE_TAR_LFO_ENV, NV_PAVS_VOICE_TAR_LFO_ENV_EA_RELEASERATE,
        NV_PAVS_VOICE_PAR_OFFSET, NV_PAVS_VOICE_PAR_OFFSET_EALVL,
        NV_PAVS_VOICE_CUR_ECNT_EACOUNT, NV_PAVS_VOICE_PAR_STATE_EACUR);
    if (ea_value < 0.0f) ea_value = 0.0f;
    if (ea_value > 1.0f) ea_value = 1.0f;

    float samples[NUM_SAMPLES_PER_FRAME][2];
    memset(samples, 0, sizeof(samples));

    bool multipass = voice_get_mask(d, v, NV_PAVS_VOICE_CFG_FMT,
                                    NV_PAVS_VOICE_CFG_FMT_MULTIPASS) != 0;
    dbg->multipass = multipass;

    if (multipass) {
        /* Read from multipass bin */
        int mp_bin = voice_get_mask(d, v, NV_PAVS_VOICE_CFG_FMT,
                                    NV_PAVS_VOICE_CFG_FMT_MULTIPASS_BIN);
        dbg->multipass_bin = (uint8_t)mp_bin;
        for (int i = 0; i < NUM_SAMPLES_PER_FRAME; i++) {
            samples[i][0] = mixbins[mp_bin][i];
            samples[i][1] = mixbins[mp_bin][i];
        }
        bool clear_mix = voice_get_mask(d, v, NV_PAVS_VOICE_CFG_FMT,
                                        NV_PAVS_VOICE_CFG_FMT_CLEAR_MIX) != 0;
        if (clear_mix) {
            memset(&mixbins[mp_bin][0], 0, sizeof(mixbins[0]));
        }
    } else {
        for (int sample_count = 0; sample_count < NUM_SAMPLES_PER_FRAME;) {
            int active = voice_get_mask(d, v, NV_PAVS_VOICE_PAR_STATE,
                                        NV_PAVS_VOICE_PAR_STATE_ACTIVE_VOICE);
            if (!active) return;
            int count = voice_resample(d, v, &samples[sample_count],
                                       NUM_SAMPLES_PER_FRAME - sample_count, rate);
            if (count < 0) break;
            sample_count += count;
        }
    }

    int active = voice_get_mask(d, v, NV_PAVS_VOICE_PAR_STATE,
                                NV_PAVS_VOICE_PAR_STATE_ACTIVE_VOICE);
    if (!active) return;

    /* Get volume bins */
    int bin[8];
    bin[0] = voice_get_mask(d, v, NV_PAVS_VOICE_CFG_VBIN, NV_PAVS_VOICE_CFG_VBIN_V0BIN);
    bin[1] = voice_get_mask(d, v, NV_PAVS_VOICE_CFG_VBIN, NV_PAVS_VOICE_CFG_VBIN_V1BIN);
    bin[2] = voice_get_mask(d, v, NV_PAVS_VOICE_CFG_VBIN, NV_PAVS_VOICE_CFG_VBIN_V2BIN);
    bin[3] = voice_get_mask(d, v, NV_PAVS_VOICE_CFG_VBIN, NV_PAVS_VOICE_CFG_VBIN_V3BIN);
    bin[4] = voice_get_mask(d, v, NV_PAVS_VOICE_CFG_VBIN, NV_PAVS_VOICE_CFG_VBIN_V4BIN);
    bin[5] = voice_get_mask(d, v, NV_PAVS_VOICE_CFG_VBIN, NV_PAVS_VOICE_CFG_VBIN_V5BIN);
    bin[6] = voice_get_mask(d, v, NV_PAVS_VOICE_CFG_FMT, NV_PAVS_VOICE_CFG_FMT_V6BIN);
    bin[7] = voice_get_mask(d, v, NV_PAVS_VOICE_CFG_FMT, NV_PAVS_VOICE_CFG_FMT_V7BIN);

    if (v < MCPX_HW_MAX_3D_VOICES) {
        bin[0] = d->vp.hrtf_submix[0];
        bin[1] = d->vp.hrtf_submix[1];
        bin[2] = d->vp.hrtf_submix[2];
        bin[3] = d->vp.hrtf_submix[3];
    }

    uint16_t vol[8];
    vol[0] = (uint16_t)voice_get_mask(d, v, NV_PAVS_VOICE_TAR_VOLA, NV_PAVS_VOICE_TAR_VOLA_VOLUME0);
    vol[1] = (uint16_t)voice_get_mask(d, v, NV_PAVS_VOICE_TAR_VOLA, NV_PAVS_VOICE_TAR_VOLA_VOLUME1);
    vol[2] = (uint16_t)voice_get_mask(d, v, NV_PAVS_VOICE_TAR_VOLB, NV_PAVS_VOICE_TAR_VOLB_VOLUME2);
    vol[3] = (uint16_t)voice_get_mask(d, v, NV_PAVS_VOICE_TAR_VOLB, NV_PAVS_VOICE_TAR_VOLB_VOLUME3);
    vol[4] = (uint16_t)voice_get_mask(d, v, NV_PAVS_VOICE_TAR_VOLC, NV_PAVS_VOICE_TAR_VOLC_VOLUME4);
    vol[5] = (uint16_t)voice_get_mask(d, v, NV_PAVS_VOICE_TAR_VOLC, NV_PAVS_VOICE_TAR_VOLC_VOLUME5);
    vol[6] = (uint16_t)(voice_get_mask(d, v, NV_PAVS_VOICE_TAR_VOLC, NV_PAVS_VOICE_TAR_VOLC_VOLUME6_B11_8) << 8);
    vol[6] |= (uint16_t)(voice_get_mask(d, v, NV_PAVS_VOICE_TAR_VOLB, NV_PAVS_VOICE_TAR_VOLB_VOLUME6_B7_4) << 4);
    vol[6] |= (uint16_t)voice_get_mask(d, v, NV_PAVS_VOICE_TAR_VOLA, NV_PAVS_VOICE_TAR_VOLA_VOLUME6_B3_0);
    vol[7] = (uint16_t)(voice_get_mask(d, v, NV_PAVS_VOICE_TAR_VOLC, NV_PAVS_VOICE_TAR_VOLC_VOLUME7_B11_8) << 8);
    vol[7] |= (uint16_t)(voice_get_mask(d, v, NV_PAVS_VOICE_TAR_VOLB, NV_PAVS_VOICE_TAR_VOLB_VOLUME7_B7_4) << 4);
    vol[7] |= (uint16_t)voice_get_mask(d, v, NV_PAVS_VOICE_TAR_VOLA, NV_PAVS_VOICE_TAR_VOLA_VOLUME7_B3_0);

    for (int i = 0; i < 8; i++) {
        dbg->bin[i] = (uint8_t)bin[i];
        dbg->vol[i] = vol[i];
    }

    if (voice_should_mute(v)) return;

    /* Low-pass filter */
    int fmode = voice_get_mask(d, v, NV_PAVS_VOICE_CFG_MISC,
                               NV_PAVS_VOICE_CFG_MISC_FMODE);
    bool lpf = false;
    if (v < MCPX_HW_MAX_3D_VOICES) {
        lpf = (fmode == 1);
    } else {
        lpf = stereo ? (fmode == 1) : (fmode & 1) != 0;
    }
    if (lpf) {
        for (int ch = 0; ch < 2; ch++) {
            int16_t fc = (int16_t)voice_get_mask(
                d, v, NV_PAVS_VOICE_TAR_FCA + (ch % channels) * 4,
                NV_PAVS_VOICE_TAR_FCA_FC0);
            float fc_f = clampf(powf(2, fc / 4096.0f), 0.003906f, 1.0f);
            uint16_t q = (uint16_t)voice_get_mask(
                d, v, NV_PAVS_VOICE_TAR_FCA + (ch % channels) * 4,
                NV_PAVS_VOICE_TAR_FCA_FC1);
            float q_f = clampf(q / (1.0f * 0x8000), 0.079407f, 1.0f);
            sv_filter *filter = &d->vp.filters[v].svf[ch];
            setup_svf(filter, fc_f, q_f, F_LP);
            for (int i = 0; i < NUM_SAMPLES_PER_FRAME; i++) {
                samples[i][ch] = run_svf(filter, samples[i][ch]);
                samples[i][ch] = fminf(fmaxf(samples[i][ch], -1.0f), 1.0f);
            }
        }
    }

    /* HRTF processing for 3D voices */
    if (v < MCPX_HW_MAX_3D_VOICES && g_config.audio.hrtf) {
        uint16_t hrtf_handle =
            (uint16_t)voice_get_mask(d, v, NV_PAVS_VOICE_CFG_HRTF_TARGET,
                                     NV_PAVS_VOICE_CFG_HRTF_TARGET_HANDLE);
        if (hrtf_handle != HRTF_NULL_HANDLE) {
            hrtf_filter_process(&d->vp.filters[v].hrtf, samples, samples);
        }
    }

    /* Mix into bins */
    for (int b = 0; b < 8; b++) {
        float g = ea_value;
        float hr;
        if ((v < MCPX_HW_MAX_3D_VOICES) && (b < 4)) {
            hr = (float)(1 << d->vp.hrtf_headroom);
        } else {
            hr = (float)(1 << d->vp.submix_headroom[bin[b]]);
        }
        g *= attenuate(vol[b]) / hr;
        for (int i = 0; i < NUM_SAMPLES_PER_FRAME; i++) {
            mixbins[bin[b]][i] += g * samples[i][b % channels];
        }
    }

    /* What this voice actually produced, before any mixbin routing or volume.
     *
     * "The voice ran" and "the voice made a sound" are different claims, and the
     * gap at the music change turns on exactly that difference: the guest covers
     * the change with a second voice, that voice is processed for the right
     * number of frames, and the output is silent for the whole window anyway.
     * This separates a voice that fetched zeros from one whose samples were fine
     * and got lost downstream in the bins or the volume. */
    if (voice_rate_on() && v < MCPX_HW_MAX_VOICES) {
        double e = 0.0;
        int si;
        for (si = 0; si < NUM_SAMPLES_PER_FRAME; ++si)
            e += fabs((double)samples[si][0]) + fabs((double)samples[si][1]);
        g_voice_rate[v].energy += e;
        if (e < 1e-6) g_voice_rate[v].silent_frames++;
    }

    /* VP monitor mix */
    if (d->monitor.point == MCPX_APU_DEBUG_MON_VP) {
        float g = 0.0f;
        for (int b = 0; b < 8; b++) {
            float hr = (float)(1 << d->vp.submix_headroom[bin[b]]);
            float bg = attenuate(vol[b]) / hr;
            if (bg > g) g = bg;
        }
        g *= ea_value;
        for (int i = 0; i < NUM_SAMPLES_PER_FRAME; i++) {
            sample_buf[i][0] += g * samples[i][0];
            sample_buf[i][1] += g * samples[i][1];
        }
    }

    (void)voice_list;
}

/* ============================================================
 * VP Frame - Process all voice lists
 *
 * Simplified single-threaded version (no worker threads initially)
 * ============================================================ */

void mcpx_apu_vp_frame(MCPXAPUState *d,
                        float mixbins[NUM_MIXBINS][NUM_SAMPLES_PER_FRAME])
{
    memset(d->vp.sample_buf, 0, sizeof(d->vp.sample_buf));

    for (int list = 0; list < 3; list++) {
        hwaddr top, current, next;
        top = voice_list_regs[list].top;
        current = voice_list_regs[list].current;
        next = voice_list_regs[list].next;

        d->regs[current] = d->regs[top];

        for (int i = 0; d->regs[current] != 0xFFFF; i++) {
            if (i >= MCPX_HW_MAX_VOICES) {
                DPRINTF("Voice list contains invalid entry!\n");
                break;
            }

            uint16_t v = (uint16_t)d->regs[current];

            /* NEXT_VOICE_HANDLE is a full 16-bit field (mask 0x0000FFFF) and
             * the terminator is 0xFFFF, so the loop condition above admits
             * every value from 0x0100 to 0xFFFE -- all of which are invalid,
             * because the hardware has 256 voices. The only thing standing
             * between such a handle and voice_process was an assert, and
             * voice_process indexes g_dbg.vp.v[256] with it before doing
             * anything else: fatal in a debug build, an out-of-bounds write in
             * a release one.
             *
             * Reached for the first time on the Windows host once it got far
             * enough to load media. The value is reported rather than assumed,
             * because "the guest wrote a bad handle" and "we decoded the link
             * field wrongly" want different fixes and only the number tells
             * them apart. Bounded: a list that is wrong once is wrong every
             * frame. */
            if (v >= MCPX_HW_MAX_VOICES) {
                static unsigned reported;
                if (reported < 8) {
                    reported++;
                    fprintf(stderr,
                            "  [APU] voice list %d entry %d: handle 0x%04X is "
                            "out of range (max %d, terminator 0xFFFF) -- "
                            "stopping this list\n",
                            list, i, v, MCPX_HW_MAX_VOICES - 1);
                    fflush(stderr);
                }
                break;
            }

            d->regs[next] = voice_get_mask(d, v, NV_PAVS_VOICE_TAR_PITCH_LINK,
                               NV_PAVS_VOICE_TAR_PITCH_LINK_NEXT_VOICE_HANDLE);

            if (!voice_get_mask(d, v, NV_PAVS_VOICE_PAR_STATE,
                                NV_PAVS_VOICE_PAR_STATE_ACTIVE_VOICE)) {
                fe_method(d, SE2FE_IDLE_VOICE, v);
                /* Keep the decoded idle voice stable until the guest services
                 * the trap; walking another voice would overwrite its payload. */
                if ((d->regs[NV_PAPU_FECTL] & NV_PAPU_FECTL_FEMETHMODE) ==
                        NV_PAPU_FECTL_FEMETHMODE_TRAPPED)
                    return;
            } else {
                /* Process voice directly (single-threaded) */
                g_apu_voice_process_count++;
                voice_process(d, mixbins, d->vp.sample_buf, v, list);
            }
            d->regs[current] = d->regs[next];
        }
    }

    /* VP monitor output */
    if (d->monitor.point == MCPX_APU_DEBUG_MON_VP) {
        int16_t isamp[NUM_SAMPLES_PER_FRAME * 2];
        src_float_to_short_array((float *)d->vp.sample_buf, isamp,
                                 NUM_SAMPLES_PER_FRAME * 2);
        int off = (d->ep_frame_div % 8) * NUM_SAMPLES_PER_FRAME;
        for (int i = 0; i < NUM_SAMPLES_PER_FRAME; i++) {
            d->monitor.frame_buf[off + i][0] += isamp[2 * i];
            d->monitor.frame_buf[off + i][1] += isamp[2 * i + 1];
        }
        memset(d->vp.sample_buf, 0, sizeof(d->vp.sample_buf));
        memset(mixbins, 0, sizeof(float) * NUM_MIXBINS * NUM_SAMPLES_PER_FRAME);
    }
}

/* ============================================================
 * VP Init / Finalize / Reset
 * ============================================================ */

void mcpx_apu_vp_init(MCPXAPUState *d)
{
    /* Single-threaded - no worker dispatch needed */
    (void)d;
}

void mcpx_apu_vp_finalize(MCPXAPUState *d)
{
    (void)d;
}

void mcpx_apu_vp_reset(MCPXAPUState *d)
{
    d->vp.ssl_base_page = 0;
    d->vp.hrtf_headroom = 0;
    memset(d->vp.ssl, 0, sizeof(d->vp.ssl));
    memset(d->vp.hrtf_submix, 0, sizeof(d->vp.hrtf_submix));
    memset(d->vp.submix_headroom, 0, sizeof(d->vp.submix_headroom));
    memset(d->vp.voice_locked, 0, sizeof(d->vp.voice_locked));
    for (int v = 0; v < MCPX_HW_MAX_VOICES; v++) {
        hrtf_filter_init(&d->vp.filters[v].hrtf);
    }
}
