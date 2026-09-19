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
/* recomp_switch_on: one grammar for the switches, so a new one does not fail
 * the jsrf_switch_audit ratchet by hand-rolling its own getenv. */
#include "../recomp_switch.h"
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

/* THE LIST CURSOR. See the long comment at mcpx_apu_cursor_pin in apu_core.c:
 * Microsoft's model writes 0xFFFF into the CVL and NVL registers at reset and
 * never touches them again, while JSRF's removal routine reads ours and takes
 * a repair branch on it. */
int mcpx_apu_cursor_pin(void);
extern unsigned long g_apu_cursor_published;
extern unsigned long g_apu_cursor_pinned;

/* Publish a cursor, unless pinned. Counted in BOTH arms: `published` is what
 * the other arm would withhold, so one control run sizes the A/B. */
#define CURSOR_SET(reg_, val_) do {                                          \
        if (mcpx_apu_cursor_pin()) { ++g_apu_cursor_pinned; }                \
        else { d->regs[(reg_)] = (val_); ++g_apu_cursor_published; }         \
    } while (0)

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

/* off counts every model retirement, including envelope/sample exhaustion.
 * Explicit guest VOICE_OFF commands are counted separately below. Neither
 * counter establishes that the guest freed its software voice object. */
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

/* Defined beside the ADPCM extent probe; written by the two PIO methods that
 * declare a voice's buffer, which are handled long before that point. */
extern uint32_t g_voice_guest_ebo[MCPX_HW_MAX_VOICES];
extern uint32_t g_voice_guest_ba[MCPX_HW_MAX_VOICES];
extern uint32_t g_voice_guest_ebo_sets[MCPX_HW_MAX_VOICES];
extern uint32_t g_voice_guest_ba_sets[MCPX_HW_MAX_VOICES];

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

/* A bounded, opt-in lifecycle log. Record the first idle encounter after
 * each ON, plus each command/retirement. Repeated traps cannot evict the
 * transition that started a storm. Sequence is log order; audio_frames is a
 * capture position, not wall time. This observes hardware state only: guest
 * object ownership must be checked separately. */
static void voice_lifecycle_note(MCPXAPUState *d, uint16_t v,
                                 const char *event)
{
    static int enabled = -1;
    static unsigned long sequence;
    static unsigned char idle_seen[MCPX_HW_MAX_VOICES];
    extern unsigned long long g_apu_out_frames;
    if (enabled < 0) enabled = getenv("RECOMP_VOICE_LIFECYCLE") != NULL;
    if (!enabled || v >= MCPX_HW_MAX_VOICES) return;
    if (!strcmp(event, "on")) idle_seen[v] = 0;
    if (!strcmp(event, "idle")) {
        if (idle_seen[v]) return;
        idle_seen[v] = 1;
    }
    fprintf(stderr, "  [VOICE-LIFECYCLE] seq=%lu audio_frames=%llu"
            " event=%s voice=%u state=%08X next=%04X"
            " fectl=%08X force1=%08X\n",
            ++sequence, g_apu_out_frames, event, v,
            voice_get_mask(d, v, NV_PAVS_VOICE_PAR_STATE, 0xFFFFFFFF),
            voice_get_mask(d, v, NV_PAVS_VOICE_TAR_PITCH_LINK,
                           NV_PAVS_VOICE_TAR_PITCH_LINK_NEXT_VOICE_HANDLE),
            d->regs[NV_PAPU_FECTL], d->regs[NV_PAPU_FETFORCE1]);
}

/* Who else writes the voice list?
 *
 * voice_set_mask stores through stl_le_phys: the voice register file lives in
 * GUEST RAM, not in d->regs. So the guest changes a PITCH_LINK with an ordinary
 * store -- no MMIO, no method, nothing a write hook can see, and nothing that a
 * trapped-page window could swallow. Only the three list HEADS are registers.
 *
 * That makes a write trace impossible and a differential one easy: remember the
 * value this model last wrote for each voice, and whenever we read that link
 * back, report it if it no longer matches. A mismatch is someone else's store,
 * and the only other writer is the guest.
 *
 * Reports the first divergence per voice rather than every one: the walk reads
 * every link every subframe, so an unbounded log would be tens of thousands of
 * lines a second once a list is being edited. */
static uint16_t g_link_shadow[MCPX_HW_MAX_VOICES];
static uint8_t  g_link_shadow_valid[MCPX_HW_MAX_VOICES];
static uint8_t  g_link_reported[MCPX_HW_MAX_VOICES];
unsigned long   g_guest_link_writes;
unsigned long   g_link_checks;

static void link_shadow_set(uint16_t v, uint32_t val)
{
    if (v >= MCPX_HW_MAX_VOICES) return;
    g_link_shadow[v] = (uint16_t)val;
    g_link_shadow_valid[v] = 1;
}

static void link_shadow_check(uint16_t v, uint32_t observed, const char *where)
{
    static int enabled = -1;
    extern unsigned long long g_apu_out_frames;
    if (v >= MCPX_HW_MAX_VOICES || !g_link_shadow_valid[v]) return;
    g_link_checks++;
    if ((uint16_t)observed == g_link_shadow[v]) return;
    g_guest_link_writes++;
    if (enabled < 0) enabled = getenv("RECOMP_VOICE_LIFECYCLE") != NULL;
    if (enabled && !g_link_reported[v]) {
        g_link_reported[v] = 1;
        fprintf(stderr, "  [VOICE-RELINK] audio_frames=%llu voice=%u"
                " ours=%04X now=%04X at=%s\n",
                g_apu_out_frames, v, g_link_shadow[v],
                (unsigned)(observed & 0xFFFF), where);
    }
    g_link_shadow[v] = (uint16_t)observed;   /* resync; report transitions once */
}

/* Every VOICE_ON insert, under the same switch as the lifecycle trace so the
 * two interleave in one log. Prints the FEAV the insert read, the branch it
 * chose, and the link before and after -- enough to read a self-link back to
 * the register that produced it without re-deriving the arithmetic by hand.
 * Unbounded on purpose: ONs are thousands per run, not the hundreds of
 * thousands the idle traps reach, and losing the early ones would lose the
 * insert that started a storm. */
static void voice_link_note(MCPXAPUState *d, uint16_t v, uint32_t feav,
                            unsigned int list, unsigned int ante,
                            uint32_t link_before, uint32_t link_after)
{
    static int enabled = -1;
    extern unsigned long long g_apu_out_frames;
    if (enabled < 0) enabled = getenv("RECOMP_VOICE_LIFECYCLE") != NULL;
    if (!enabled || v >= MCPX_HW_MAX_VOICES) return;
    (void)d;
    fprintf(stderr, "  [VOICE-LINK] audio_frames=%llu voice=%u feav=%08X"
            " lst=%u ante=%04X link=%04X->%04X%s%s\n",
            g_apu_out_frames, v, feav, list, ante,
            link_before, link_after,
            (list == NV1BA0_PIO_SET_ANTECEDENT_VOICE_LIST_INHERIT &&
             ante == v) ? " SELF-ANTE" : "",
            (link_after == v) ? " SELF-LINK" : "");
}

/* Idle-voice traps, by handle, and the last sixteen raised.
 *
 * The guest's DirectSound ISR takes this handle, looks up its own voice object
 * and dereferences it -- and 20 of the 33 genuine faults on this host are that
 * dereference finding NULL. A live stack walk has since put 0x001A2450 and
 * 0x001A2031 on the faulting thread's stack, which are that ISR's frames. What
 * is NOT established is whether the trap is raised for a voice the title has
 * already torn down; this records what we last asked the guest to service so a
 * crash can be read against it. */
unsigned long g_idle_trap_raises;
unsigned long g_idle_trap_by_voice[MCPX_HW_MAX_VOICES];
uint16_t g_idle_trap_last[16];
unsigned long g_idle_trap_ring;

/* WHAT THE HANDLE WAS, not just which handle it was.
 *
 * The ring above names the voice. It cannot say whether that voice was one the
 * guest had finished setting up, which is the whole question: the ISR chain
 * this handle enters is
 *
 *   001A25AA  reads FECTL, FEDECMETH, FEDECPARAM; dispatches if trapped
 *   001A24BE  if (method != 0x8000) return          -- SE2FE_IDLE_VOICE
 *   001A241F  if (h >= 0x100) return; if (this->+0x2C0) return;
 *             if (voicereg[h].CFG_FMT & 0x800000) return  -- PERSIST, +4
 *   001A200D  pBuf = this->owner[h] (+0x2C4 + h*4);  NO NULL CHECK
 *   001A2E2E  pBuf->... -- the fault, with pBuf == NULL
 *
 * -- read out of the generated C and confirmed against 40 crash dumps, whose
 * WORKER GUEST STACK CODE POINTERS carry 001A25D9 / 001A24D1 / 001A2450 /
 * 001A2031, the return addresses of exactly those four call sites, and whose
 * six guest registers are reproduced by that path and no other in the function.
 *
 * So the guest dereferences owner[h] unguarded, and it is entitled to: on
 * hardware a handle only reaches this chain if DirectSound put the voice in a
 * list, which it does after it has an owner for it. The interesting facts
 * about a raise are therefore whether the voice was LOCKED (the guest is
 * mid-VOICE_ON or mid-RELEASE on it), whether it had EVER been VOICE_ON in
 * this run at all, and what its CFG_FMT read -- a zero fmt is an unconfigured
 * voice, which is also a voice with no owner and no PERSIST bit to save it.
 *
 * Kept in parallel arrays rather than widening g_idle_trap_last, so the
 * existing crash-dump line and everything that greps for it are unchanged.
 * 16 slots costs 128 bytes and one branchless store per raise. */
#define IDLE_TRAP_WHY_LOCKED   (1u << 0)  /* voice_locked bit set at the raise */
#define IDLE_TRAP_WHY_NEVER_ON (1u << 1)  /* no VOICE_ON for this handle, ever */
#define IDLE_TRAP_WHY_PERSIST  (1u << 2)  /* CFG_FMT PERSIST -- the ISR returns */
#define IDLE_TRAP_WHY_REPEAT   (1u << 3)  /* same handle as the previous raise */
/* THE FLAG THE FATAL ENTRY WAS MISSING. A run's last ring entry has so far
 * carried no flags at all -- not locked, not never-on -- which is what says a
 * third mechanism exists. This one asks the remaining question: was this voice
 * ever found twice in a single list walk, i.e. is it in a ring our TVL will go
 * on naming after the guest has removed it? Sticky per voice, set by the walk's
 * cycle detector, so reading it here costs a bitmap test. */
#define IDLE_TRAP_WHY_CYCLE    (1u << 4)  /* handle seen twice in one walk */
#define IDLE_TRAP_WHY_UNOWNED  (1u << 5)  /* guest does not own it NOW */
uint8_t  g_idle_trap_why[16];
uint16_t g_idle_trap_from[16];   /* predecessor handle, 0xFFFF = straight off TVL */
uint32_t g_idle_trap_fmt[16];    /* the voice's CFG_FMT as the raise found it */
uint8_t  g_idle_trap_list[16];   /* 0 = 2D, 1 = 3D, 2 = MP */

/* Totals for the same three facts, because a 16-slot ring only survives the
 * last few milliseconds and the periodic report has to be able to say "this
 * has been happening all run" or "it has never happened".
 *
 * g_idle_trap_raises is the positive control for all three: a zero here beside
 * a zero there says only that nothing trapped, which proves nothing about the
 * instrument. Read them as a pair. */
/* TWO COUNTERS, BECAUSE THEY HAVE DIFFERENT DENOMINATORS AND ONE OF THEM ONCE
 * MASQUERADED AS THE OTHER.
 *
 * _encounters is every time the walk finds an inactive voice whose lock the
 * guest holds -- 1500 Hz, per voice, per list, whether or not a raise follows.
 * _raises is the subset that actually became an SE2FE_IDLE_VOICE.
 *
 * They were one counter, incremented outside the raise gate and printed beside
 * `raises` as though it were a subset of it. It is not, and it is not even
 * bounded by it: 42 of the 152 distinct [APU-IDLE-TRAP] lines in build-macos
 * read locked > raises, e.g. `raises=10536 locked=16384`. The "56% of raises
 * happen while locked" figure that justified the lock guard was 13,294
 * ENCOUNTERS over 23,933 RAISES -- a ratio of two different things, and not a
 * percentage of anything. Found by audit, 16 Sep 2026; see the guard's own
 * comment above, which has been corrected. */
unsigned long g_idle_trap_locked_encounters;
unsigned long g_idle_trap_locked_raises;
unsigned long g_idle_trap_never_on_raises;
unsigned long g_idle_trap_repeat_raises;

/* THE RAISE THE GUEST'S ISR IS WRITTEN TO IGNORE, WHICH NOTHING HAS EVER
 * COUNTED.
 *
 * The ISR chain transcribed at the top of this file ends its guard sequence
 * with, at 001A241F:
 *
 *     if (voicereg[h].CFG_FMT & 0x800000) return;      -- PERSIST
 *
 * so for a PERSIST voice DirectSound acknowledges the trap and deliberately
 * does NOTHING: it does not unlink the voice, does not clear the owner, does
 * not mark it inactive. That is correct -- PERSIST is the driver telling the
 * hardware the voice keeps its list slot across stop and start -- and it is
 * also a closed loop against a LEVEL-triggered raise, which is what this model
 * has. The walk finds the same voice inactive and still linked on the next
 * subframe, 1500 times a second, for ever.
 *
 * The P flag has been recorded in the 16-slot ring since the ring was written
 * (IDLE_TRAP_WHY_PERSIST), and the ring only survives the last few
 * milliseconds. There has never been a TOTAL, so no run in this tree can say
 * what fraction of a 43,000-raise storm was voices whose ISR was always going
 * to return. That is the one number that separates "the guest is being handed
 * the wrong handle" from "the guest is being handed the right handle and told
 * to ignore it", and the two want completely different fixes.
 *
 * Positive control: g_idle_trap_raises, printed on the same line. */
unsigned long g_idle_trap_persist_raises;

/* EDGE, NOT LEVEL: one raise per idle TRANSITION, per voice.
 *
 * One bit per voice, set when we raise for it and cleared when the walk next
 * finds it ACTIVE or when VOICE_ON restarts it. mcpx_apu_trap_coalesce already
 * withholds a raise while the front end is still TRAPPED; this withholds the
 * one that comes AFTER the guest has serviced and resumed, which is the one
 * the coalescing cannot see.
 *
 * _encounters counts only raises the rule WOULD have withheld -- it is
 * incremented at the point a raise would otherwise have happened, so it is a
 * subset of raises and not the different-denominator trap that
 * g_idle_trap_locked_encounters fell into. _suppressed counts the ones it
 * actually withheld, so a run with the switch OFF answers "would this have
 * mattered" and a run with it ON says whether it took.
 *
 * _rearm is the instrument's own liveness: a latch that is set and never
 * cleared would look exactly like a working fix while silently swallowing
 * every genuine retirement after the first. Non-zero rearm beside non-zero
 * suppressed is what says the latch is cycling rather than stuck.
 *
 * Plain loads and stores. The bitmap is touched by the frame thread (walk) and
 * by guest threads (VOICE_ON) without the APU lock, and a lost update costs at
 * most one extra raise or one withheld raise -- the same order of error as the
 * race it is reducing, and not worth putting a lock in VOICE_ON's body for. */
static uint64_t g_idle_trap_reported[MCPX_HW_MAX_VOICES / 64];
unsigned long g_idle_trap_edge_encounters;
unsigned long g_idle_trap_edge_suppressed;
unsigned long g_idle_trap_edge_rearm;
/* THE BOUNDED RE-RAISE. A latched voice that the guest has not reclaimed is
 * told again after this many withheld subframes. _reraise counts those; a
 * run where it is zero with suppressed climbing is the pure edge, which
 * silenced the player's music twice on 16 Sep 2026 (see
 * mcpx_apu_idle_trap_rearm_subframes). hold[] is per voice, plain stores,
 * same justification as the bitmap above. */
unsigned long g_idle_trap_edge_reraise;
static uint16_t g_idle_trap_edge_hold[MCPX_HW_MAX_VOICES];

/* THE VOICE POOL, so voice theft is measurable. on_active: VOICE_ON for a
 * handle whose ACTIVE_VOICE bit was still set -- the guest restarted or STOLE
 * a playing voice. reuse: VOICE_ON for a handle retired earlier in the same
 * report window. distinct: handles started this window. A healthy pool has
 * distinct >> reuse and on_active ~ 0; a guest that cannot reclaim voices
 * (no idle notification) shows distinct collapsing and on_active climbing,
 * which is what "effects cut off, then the music" sounds like. */
unsigned long g_pool_on_active, g_pool_reuse;
/* THE POSITIVE CONTROL [APU-BIN] NEVER HAD.
 *
 * "2D heard" counts subframes in which a 2D voice produced a non-zero
 * amplitude, and it reads ZERO in two completely different worlds: one where
 * we have lost the music, and one where the guest is simply not playing any
 * (a menu, a cutscene, a stretch with no BGM). On 16 Sep 2026 a player's
 * session froze 2D heard at 95,324 for twenty-six report windows and nothing
 * in the log could tell those apart -- the only way to settle it was to ask
 * the player whether they could hear music, which is not a measurement.
 *
 * on_2d counts VOICE_ON for a handle at or above MCPX_HW_MAX_3D_VOICES, i.e.
 * the guest ASKING for a 2D voice. Read it against 2D heard:
 *
 *   on_2d climbing, 2D heard frozen  -> the guest asked and we dropped it.
 *                                       OUR defect, and the voice handles in
 *                                       the ring say which.
 *   on_2d frozen too                 -> the guest played no music. NOT a
 *                                       defect; stop looking.
 *
 * on_3d is the control for the control: if both read zero the instrument is
 * dead rather than the audio. */
unsigned long g_pool_on_2d, g_pool_on_3d;
static uint64_t g_pool_on_window[MCPX_HW_MAX_VOICES / 64];
static uint64_t g_pool_off_window[MCPX_HW_MAX_VOICES / 64];
int mcpx_apu_idle_trap_edge(void);

/* Every handle the guest has ever issued VOICE_ON for, one bit each.
 *
 * Set in the VOICE_ON handler and never cleared: the question it answers is
 * "has DirectSound ever owned this voice", and a voice it has since retired
 * still has an owner slot. A handle that is still zero here when we hand it to
 * the ISR is one the guest has never seen, so owner[h] cannot be anything but
 * the NULL it was initialised to. */
uint64_t g_apu_voice_ever_on[MCPX_HW_MAX_VOICES / 64];

/* THE INVARIANT THE ISR IS ENTITLED TO RELY ON, AND WHETHER WE BREAK IT.
 *
 * 001A200D does `pBuf = this->owner[h]` with NO NULL CHECK, and the comment
 * above records why it is allowed to: "on hardware a handle only reaches this
 * chain if DirectSound put the voice in a list, which it does after it has an
 * owner for it."
 *
 * Our list and DirectSound's ownership have demonstrably diverged --
 * [APU-IDLE-DELIVERY] says we name every retired voice and the guest removes
 * almost none, so voices sit in OUR list long after the guest is done with
 * them. If ownership is dropped while the voice is still in our list, every
 * later raise for it hands the ISR a handle whose owner[] is NULL.
 *
 * That is exactly the observed fault: 26 crash dumps (18 previously recorded,
 * 8 more on 18 Sep), every one of them the owner[h]==NULL register set, and
 * every one of them with voice 0 as the last handle raised.
 *
 * ever_on cannot answer this -- it is deliberately never cleared, because it
 * answers "has the guest EVER owned this handle". This pair answers "does it
 * own it NOW", set at VOICE_ON and cleared at VOICE_RELEASE.
 *
 * COUNTER ONLY. Nothing is suppressed. The clear-on-RELEASE half is an
 * INFERENCE and it may be wrong: the VOICE_RELEASE handler sets envelope
 * release rates, which is a fade-out and not obviously an object free, so the
 * guest may well still own a releasing voice. If it does, `unowned` will read
 * 0 on a crashing run and this hypothesis dies there -- which is the point of
 * measuring before guarding. The precedent is the voice-lock guard beside it,
 * which was built the same way and which the 18 Sep crash dumps killed. */
uint64_t g_apu_voice_owned_now[MCPX_HW_MAX_VOICES / 64];

/* THE GUEST'S DIRECTSOUND OBJECT, CAPTURED BY A GEN PROBE.
 *
 * Zero until the probe from diagnostics/jsrf_first_fault/
 * instrument_dsound_this.py has fired, and zero for good in an uninstrumented
 * build -- which is exactly what a reader needs, because every use below is
 * gated on it being non-zero and the report says which it was.
 *
 * 001A200D does `pBuf = this->owner[h]` at this+0x2C4+h*4 with NO NULL CHECK,
 * and a NULL only proves fatal for handle 0: with owner[h] NULL the two loads
 * that follow read page zero, the `cmp eax, edx` compares h against 0, and any
 * h != 0 returns harmlessly. h == 0 falls through to the call that faults.
 * That is why all 26 crash dumps name voice 0 and no other. */
unsigned int g_jsrf_dsound_this;

/* IS THERE ONE DIRECTSOUND OBJECT, OR SEVERAL?
 *
 * Every owner[h] number this project has rests on that pointer, and the probe
 * that captures it writes it AT THE TOP OF THE ISR. So a raise-time read of
 * owner[h] reads through a `this` captured at some EARLIER ISR entry -- never
 * the entry about to service that raise. If the title keeps one device object
 * for the run, the staleness is harmless and every owner[] number stands. If
 * it keeps more than one, the raise-time check has been indexing the wrong
 * object's table, and "of those NULLs, 0 were h==0" -- the positive control
 * that retired the owner guard -- was measuring something else entirely.
 *
 * Nobody had checked. The plan that proposed reading owner[h] wrote its own
 * risk note -- "`this` may be per-device; the probe must catch the right one"
 * -- and then the guard was measured, refuted and defaulted off without it
 * ever being answered. Counting distinct values instead of keeping only the
 * last settles it in one session, for the cost of a scan of eight words.
 *
 * READ `calls` FIRST. distinct=0 with calls=0 means the probe is not
 * installed, NOT that the guest has no device -- the same confident zero this
 * file warns about everywhere else. distinct=1 with calls>0 is the result
 * that makes the numbers above trustworthy. Anything higher reopens them.
 *
 * Past DSOUND_THIS_MAX the table stops growing and `over` counts the
 * sightings it could not place. `over` is a yes/no signal, not a census: by
 * the time it moves, "more than one" is already the answer. */
#define DSOUND_THIS_MAX 8u
unsigned int  g_jsrf_dsound_this_seen[DSOUND_THIS_MAX];
unsigned long g_jsrf_dsound_this_distinct;
unsigned long g_jsrf_dsound_this_calls;   /* positive control for distinct=0 */
unsigned long g_jsrf_dsound_this_over;    /* sightings past the table */

/* Called from the gen-tree probe at sub_001A25AA's entry, on the GUEST thread,
 * while the report reads these from another -- the same arrangement every
 * other ring in this file already has. Wait-free: a scan of at most eight
 * words on a path that runs a few thousand times a session. */
void jsrf_dsound_this_seen(unsigned int t)
{
    unsigned long kept = g_jsrf_dsound_this_distinct < DSOUND_THIS_MAX
                       ? g_jsrf_dsound_this_distinct : DSOUND_THIS_MAX;
    unsigned long i;

    ++g_jsrf_dsound_this_calls;
    g_jsrf_dsound_this = t;
    if (!t)
        return;
    for (i = 0; i < kept; ++i)
        if (g_jsrf_dsound_this_seen[i] == t)
            return;
    if (g_jsrf_dsound_this_distinct < DSOUND_THIS_MAX) {
        g_jsrf_dsound_this_seen[g_jsrf_dsound_this_distinct] = t;
        ++g_jsrf_dsound_this_distinct;
    } else {
        ++g_jsrf_dsound_this_over;
    }
}

/* THE SAME POINTER, WITHOUT A GEN PROBE -- AND IT WAS ALWAYS IN OUR HANDS.
 *
 * Everything above is written as though `this` can only be captured by
 * patching the generated C, because "it is a thiscall parameter, never loaded
 * from a guest global". The parameter part is true. The conclusion is not:
 * the title HANDED US that pointer at boot and the kernel wrote it down.
 *
 * The APU is IRQ vector 5 (kernel_bridge.c says so where it pumps the device
 * interrupts), and an Xbox ISR is
 *
 *     BOOLEAN __stdcall ISR(PKINTERRUPT Interrupt, PVOID ServiceContext)
 *
 * so the context travels in the KINTERRUPT the title passed to
 * KeConnectInterrupt, at +4. Read out of the generation this tree builds
 * against, the whole path from that word to `this` is three instructions:
 *
 *     sub_001A2681:  ecx = MEM32(esp + 8);        <- ServiceContext
 *                    call sub_001A2638
 *     sub_001A2638:  esi = ecx;  ...  call sub_001A25AA   (ecx untouched)
 *     sub_001A25AA:  edi = ecx;                   <- `this`
 *
 * and 0x001A2681 is exactly the routine the log records for vector 5.
 *
 * THREE INDEPENDENT CONFIRMATIONS, which is why this is allowed to replace the
 * probe rather than sit beside it:
 *
 *   1. The dataflow above, which is not an inference -- ecx is never written
 *      between the ISR's prologue and 001A25AA.
 *   2. The recovered symbol, and it is an EXACT byte-signature match rather
 *      than one of the position-inferred names the table marks with a leading
 *      `~`. 001A25AA is `?HandleFETrap@CMcpxAPU@@IAEXXZ`, i.e.
 *      `protected: void __thiscall CMcpxAPU::HandleFETrap(void)`. A
 *      __thiscall with NO parameters takes its only input in ecx, so ecx is
 *      `this` by the calling convention, and the object is a CMcpxAPU -- the
 *      class that owns the voice table. The two calls this function ends with
 *      are `?SetFrontEndState@CMcpxCore@DirectSound@@IAEXW4MCPX_FE_STATE@@@Z`
 *      on `this+8`, the CMcpxCore subobject, which is only coherent if `this`
 *      is the APU object. (Its caller 001A2638 is named
 *      `~?ServiceApuInterrupt@CMcpxAPU@@IAEHXZ` -- inferred, so worth nothing
 *      on its own, but the bytes agree: it reads ISTS at 0xFE801000,
 *      acknowledges it, and dispatches on bit 0x10.)
 *   3. The runs that carried instrument_dsound_this.py report
 *      `this=0x008EA8B4`, and the same session's KeConnectInterrupt line reads
 *      `context=0x008EA8B4 vector=5`. Two captures, one value.
 *   4. The crash dump's own guest stack. Under the faulting frame it reads
 *      001A2031 / 001A2450 / 001A24D1 / 001A25D9 / 001A2660 / 001A268A and
 *      then 001BA718 -- which is the KINTERRUPT the log records for vector 5.
 *      The chain this walks up is the chain the crash walks down.
 *
 * The 0x2C4 offset is NOT from the symbols -- signature transfer recovers
 * names, never field offsets. It comes from 001A200D's own
 * `ecx = MEM32(ecx + eax*4 + 0x2C4)`, and nothing else.
 *
 * WHY IT MATTERS THAT THIS IS NOT A PROBE. Every owner[] number this project
 * has was taken from an instrumented gen tree, which means no shipped build
 * could ever act on one: the guard below was gated on a pointer that is zero
 * in every build a player runs. Resolving it here makes the check a property
 * of the model instead of a property of somebody's debugging tree.
 *
 * The probe still wins if it has fired -- it observes the value the ISR is
 * actually using, this only observes the value the ISR was registered with --
 * and a disagreement is counted rather than silently preferred.
 *
 * A WEAK DEFINITION, not a weak reference, and the difference was measured:
 * a weak *reference* still makes ld64 pull kernel_bridge.c.o out of the
 * archive to satisfy it, and that object needs recomp_lookup, which the small
 * APU unit tests do not have. jsrf_apu_pace_test failed to link on exactly
 * that. A weak DEFINITION here satisfies the reference without pulling
 * anything in, and is overridden by the kernel's strong definition in every
 * link that already contains it.
 *
 * Which of the two ran is not left to inference: the stub records that it was
 * called, and the report says so rather than printing an unexplained zero. */
#define JSRF_APU_IRQ_VECTOR 5u
extern uint32_t xbox_GetConnectedInterrupt(uint32_t vector);
/* Set only if the fallback below answered -- i.e. the kernel's interrupt
 * table is not in this link. A positive control for `this` reading zero. */
int g_jsrf_dsound_no_kernel;
#if !defined(_MSC_VER)
__attribute__((weak)) uint32_t xbox_GetConnectedInterrupt(uint32_t vector)
{
    (void)vector;
    g_jsrf_dsound_no_kernel = 1;
    return 0;
}
#endif

/* 0 nothing, 1 the gen probe, 2 the connected KINTERRUPT. Printed, because a
 * reader of an owner[] count has to know which table it came out of. */
int g_jsrf_dsound_this_src;
/* The probe and the KINTERRUPT disagreed. Non-zero retires the claim that
 * they are the same object and every count below has to be retaken. */
unsigned long g_jsrf_dsound_this_disagree;
unsigned int  g_jsrf_dsound_this_kint;

unsigned int jsrf_dsound_this(void)
{
    static unsigned int cached;
    uint32_t iv, ctx;

    /* The probe, if it fired: it sees the live thiscall, not the
     * registration. Cross-check it against the KINTERRUPT once. */
    if (g_jsrf_dsound_this) {
        if (g_jsrf_dsound_this_src != 1) {
            g_jsrf_dsound_this_src = 1;
            if (g_jsrf_dsound_this_kint
                && g_jsrf_dsound_this_kint != g_jsrf_dsound_this)
                ++g_jsrf_dsound_this_disagree;
        }
        return g_jsrf_dsound_this;
    }
    if (cached) return cached;
    iv = xbox_GetConnectedInterrupt(JSRF_APU_IRQ_VECTOR);
    if (!iv) return 0;                      /* not connected yet */
    /* The routine is the field whose correctness can be checked rather than
     * assumed -- a half-built KINTERRUPT has it zero. Same reasoning as
     * bridge_interrupt_ready, which this deliberately mirrors. */
    if (!ldl_le_phys(address_space_memory, iv + 0)) return 0;
    ctx = (uint32_t)ldl_le_phys(address_space_memory, iv + 4);
    /* owner[] is read at ctx+0x2C4+h*4 for h < 0x100, so the whole table has
     * to be inside the RAM ldl_le_phys can address. Anything else is a
     * context that is not a DirectSound object and must not be indexed. */
    if (!ctx || ((ctx + 0x2C4u + 0xFFu * 4u) & ~g_apu_ram_mask) != 0) return 0;
    cached = ctx;
    g_jsrf_dsound_this_kint = ctx;
    if (!g_jsrf_dsound_this_src) g_jsrf_dsound_this_src = 2;
    return cached;
}

unsigned long g_idle_owner_null_raises;  /* raises whose owner[h] read NULL */
unsigned long g_idle_owner_ok_raises;    /* positive control for that zero */
unsigned long g_idle_owner_null_h0;      /* ...and h==0: the FATAL combination */
unsigned long g_idle_owner_suppressed;   /* raises the guard actually withheld */

/* RECOMP_APU_IDLE_TRAP_OWNER_GUARD -- do not hand the ISR a handle it will
 * dereference into NULL.
 *
 * DEFAULT ON. The tree's rule is that a player-facing default needs a picture
 * or a listen, and this one has neither -- but it is not a player-facing
 * behaviour change. It withholds an interrupt the guest cannot service: with
 * owner[h] NULL, 001A200D reads page zero twice, compares the handle against
 * 0, and returns for every h except 0, where it falls through and faults. So
 * the raise is either a no-op or fatal, and suppressing it removes the second
 * without changing the first.
 *
 * Measured before shipping: 9 NULL owners in 27,947 raises, 0.03%, against a
 * positive control of 27,937 non-NULL. The cost of being wrong is that nine
 * voices a run go un-reclaimed; the cost of being right is half the project's
 * runs stop dying. Set =0 to restore the crash.
 *
 * DEFAULTED BACK OFF, 18 Sep, BY ITS OWN A/B. Five trials per arm:
 *
 *     GUARD=0   2 crashes in 5 runs
 *     GUARD=1   2 crashes in 5 runs      both at sub_001A2E2E
 *
 * and the positive control says why it could not have helped: "of those
 * NULLs, 0 were h==0" -- the ONLY combination that reaches the faulting call
 * never occurred in any run, so the guard had nothing fatal to withhold and
 * the crash arrived regardless.
 *
 * What that rules out is worth more than the guard was: the fatal handle is
 * NOT one of our idle-trap raises with a NULL owner. Either it does not come
 * from our raise at all -- the decode-pair race, where a guest method landing
 * between the ISR's two MMIO loads hands it 0x8000 with somebody else's
 * argument -- or owner[h] is still valid when we check it and becomes NULL
 * before the ISR reads it. Both are time-of-check/time-of-use, and both mean
 * a raise-time check is in the wrong place by construction.
 *
 * Kept, off, because the counters are the measurement and they cost nothing:
 * owner[h] at the raise is 27,937 non-NULL to 9 NULL, which is the only
 * discriminating number this investigation produced. */
static int owner_guard_on(void)
{
    static int on = -1;
    if (on < 0)
        on = recomp_switch_on("RECOMP_APU_IDLE_TRAP_OWNER_GUARD");
    return on;
}
unsigned long g_idle_unowned_raises;   /* raises for a voice not owned NOW */
unsigned long g_idle_owned_raises;     /* the positive control for that zero */

/* THE CHECK AT THE HAND-OVER, WHICH IS WHERE THE RAISE-TIME ONE SHOULD HAVE
 * BEEN ALL ALONG.
 *
 * The A/B above retired the raise-time owner guard and wrote down why: "either
 * it does not come from our raise at all ... or owner[h] is still valid when
 * we check it and becomes NULL before the ISR reads it. Both are
 * time-of-check/time-of-use, and both mean a raise-time check is in the wrong
 * place by construction." This is the other place.
 *
 * On hardware the two are the same instant. The APU asserts its PCI line and
 * the guest takes the interrupt at the next instruction boundary, so there is
 * no interval in which DirectSound can free the buffer between the hardware
 * choosing a handle and the ISR reading it. In this runtime there is:
 * pci_irq_assert is a no-op, the ISR runs only when a guest thread reaches
 * bridge_KeWaitForSingleObject, and the device-IRQ pump is on an 8 ms period.
 * The handle we chose can be up to 8 ms stale by the time it is collected.
 *
 * So the hand-over is not the raise. It is the guest's LOAD of FEDECMETH --
 * 001A25AA's `ecx = MEM32(0xFE801300)`, which on this host faults into
 * mcpx_apu_read. That load is the last instant at which the model still owns
 * the decision, and it happens on the guest's own thread microseconds before
 * 001A200D indexes owner[h]. Checking there closes the window that a
 * raise-time check cannot.
 *
 * IT DEPENDS ON RECOMP_APU_FEDEC_HOLD, which is default ON. The handle this
 * reads is FEDECPARAM, taken at the FEDECMETH load rather than at the guest's
 * own second load, and the two are only guaranteed to be the same pair
 * because a trapped front end holds the pair still. With FEDEC_HOLD=0 the
 * pair can be overwritten between the guest's two loads -- which is the
 * decode-pair race that switch exists for -- and this check would then be
 * validating a handle the ISR is not about to use. Run them together or not
 * at all; the report prints both states.
 *
 * WHAT IS WITHHELD. Not the interrupt, and not the handle: the METHOD. A
 * method that is not 0x8000 makes 001A24BE return without calling 001A241F,
 * which is the same nothing the ISR already does for h >= 0x100 and for a
 * PERSIST voice. The rest of 001A25AA -- the two sub_001A5B27 calls on
 * this+8 that resume the front end -- runs unchanged, so nothing is left
 * latched.
 *
 * IS NULL REALLY THE ONLY INVALID STATE? Asked because this title writes
 * sentinels into its own fields -- 0xFFEEFFEE and 0xFF555555 have both come
 * back as wild pointers from the scene-graph walker, and 0xFFEEFFEE appears
 * in the XBE itself -- so a slot holding a sentinel would sail through a test
 * that only compares against zero. Checked against the disassembly rather
 * than assumed, and for owner[] the answer is yes, NULL is the whole of it.
 * There are exactly three writers of `this+0x2C4+h*4` in the image:
 *
 *   001A1D9D, 001A1DD6 (sub_001A1CED)  owner[h] = pBuf, a real object
 *   001A1E37 (sub_001A1E01)            `and dword ptr [esi+ecx*4+0x2C4], 0`
 *
 * and the allocator in 001A1CED picks a slot by scanning for `owner[h] == edx`
 * with edx set by `xor edx, edx` and never reassigned. The guest's own free
 * marker is the literal zero, so testing for it is complete.
 *
 * WHY THE ISR IS ENTITLED TO SKIP THE CHECK, finally read off the guest's own
 * code rather than argued. sub_001A1E01 is the teardown, and it does:
 *
 *     this->0x2C0 += 1                       <- an interlock
 *     for each voice of the buffer:
 *         pBuf->[0xC + i*2] |= 0xFFFF        <- handle retired
 *         this->owner[h] = 0
 *     this->0x2C0 -= 1
 *
 * and 001A241F -- one frame above the unguarded read -- begins
 * `if (this->0x2C0 != 0) return`. So on hardware an idle-voice trap that
 * overlaps the teardown is refused outright, and one that arrives afterwards
 * names a handle the teardown has already struck out of the guest's own list.
 * Neither can reach 001A200D with a NULL. Ours can, on both counts: the raise
 * comes from OUR voice list, which the guest does not maintain, and it is
 * delivered late enough that 0x2C0 is back to zero by the time it lands.
 *
 * COUNTERS FIRST, and they answer the question the raise-time guard could not.
 * _reads is the positive control: it counts every IDLE_VOICE method the guest
 * collected, so a NULL count of zero beside a _reads of zero means the read
 * trap never fired and says nothing at all. _null_h0 is the whole hypothesis
 * -- h==0 with owner[0] NULL is the only combination that reaches
 * sub_001A2E2E, it is what all 26 crash dumps are, and the raise-time probe
 * measured it as never happening. If it is non-zero HERE while the raise-time
 * count is still zero, the TOCTOU mechanism is measured rather than argued.
 *
 * RECOMP_APU_IDLE_HANDOFF_GUARD, default OFF until a run says _null_h0 moves.
 * Defaulting it on before that would be shipping the same argument that put
 * the raise-time guard in and took it out again. */
unsigned long g_idle_handoff_reads;     /* IDLE_VOICE methods collected */
unsigned long g_idle_handoff_owned;     /* ...with a live owner[h] */
unsigned long g_idle_handoff_null;      /* ...with owner[h] == NULL */
unsigned long g_idle_handoff_null_h0;   /* ...and h == 0: the fatal one */
unsigned long g_idle_handoff_withheld;  /* methods the guard actually blanked */
unsigned long g_idle_handoff_nothis;    /* collected before `this` resolved */

static int idle_handoff_guard_on(void)
{
    static int on = -1;
    if (on < 0)
        on = recomp_switch_on("RECOMP_APU_IDLE_HANDOFF_GUARD");
    return on;
}

/* RESOLVE THE SWITCH BEFORE A FAULT CAN ARRIVE, because the place that asks
 * is a signal handler.
 *
 * mcpx_apu_read runs inside the AArch64 MMIO trap, on the guest's own thread,
 * and getenv is not async-signal-safe. xbox_memory_layout.c hit this first
 * and wrote the rule down where it resolves the USB diagnostics: "first use
 * is inside the write trap's signal handler, where getenv is not
 * async-signal-safe". A lazy static here would do the first getenv in exactly
 * that context. Called once from mcpx_apu_init_standalone, on the main
 * thread, before the aperture is guarded. */
void mcpx_apu_idle_handoff_init(void)
{
    (void)idle_handoff_guard_on();
}

/* Called from mcpx_apu_read on the guest thread, inside the read trap, for
 * the FEDECMETH load only. Read-only with respect to the guest. */
uint32_t mcpx_apu_idle_handoff_method(void *opaque, uint32_t method)
{
    MCPXAPUState *d = (MCPXAPUState *)opaque;
    unsigned int self;
    uint32_t h, owner;

    if (method != SE2FE_IDLE_VOICE || !d) return method;
    ++g_idle_handoff_reads;
    self = jsrf_dsound_this();
    if (!self) { ++g_idle_handoff_nothis; return method; }
    h = qatomic_read(&d->regs[NV_PAPU_FEDECPARAM]);
    /* 001A241F's own bound. Above it the ISR returns before touching
     * owner[], so there is nothing here to withhold. */
    if (h >= 0x100) return method;
    owner = (uint32_t)ldl_le_phys(address_space_memory,
                                  self + 0x2C4u + h * 4u);
    if (owner) { ++g_idle_handoff_owned; return method; }
    ++g_idle_handoff_null;
    if (h == 0) ++g_idle_handoff_null_h0;
    if (!idle_handoff_guard_on()) return method;
    ++g_idle_handoff_withheld;
    return 0;
}

/* THE WINDOW THIS SWITCH CLOSES, AND WHY IT IS A SWITCH.
 *
 * NV1BA0_PIO_VOICE_LOCK is the driver telling the hardware "do not look at
 * this voice, I am editing it". We implement the register -- voice_lock() sets
 * the bit, is_voice_locked() reads it -- and the VOICE_ON and VOICE_RELEASE
 * handlers take it across their own bodies. Nothing in the voice-list walk has
 * ever read it. Taking a lock and never honouring it is the whole of the
 * mechanism proposed here.
 *
 * It matters because our VOICE_ON publishes before it activates:
 *
 *     d->regs[top_reg] = selected_handle;      <- the voice is now the head
 *     ... ~80 lines: CBO, SSL, EACUR/EFCUR, ECNT, filters ...
 *     voice_set_mask(..., ACTIVE_VOICE, 1);    <- only now is it active
 *
 * and the whole of that body runs on a GUEST thread without d->lock held
 * (voice_lock takes and drops the lock around the bitmap store, nothing more),
 * while the walk runs on the frame thread. A walk that starts inside that gap
 * reads regs[top], finds the new voice with ACTIVE_VOICE still clear, and
 * raises SE2FE_IDLE_VOICE for a handle DirectSound has not finished
 * introducing -- and therefore has no owner[] entry for yet.
 *
 * WHAT IS MEASURED AND WHAT IS NOT. Measured: in all 18 crash dumps that
 * carry the ring, the LAST handle raised is voice 0, and the faulting register
 * set is the owner[h] == NULL path. Measured, in a surviving run
 * (build-macos/jsrf-first-fault/measure/DESC2): idle_trap is 0 at the t=30 s
 * report and 3 at the t=40 s one -- the first retirements of the whole run --
 * and voice 0's first VOICE_ON is the next VOICE-DESC line after that, inside
 * the t=40..50 s window. The crash window is t=34..41 s. So the fault
 * straddles the moment the title first retires voices and first reaches for
 * voice 0, which is the coincidence that made this hypothesis worth writing
 * down. NOT measured: that a raise and a VOICE_ON actually overlap, which is
 * the claim itself, and which needs a run.
 *
 * So this ships OFF. The counters below decide it in ONE run without it: every
 * raise records whether the guard WOULD have suppressed it, so a single
 * faulting run either shows the fatal raise flagged L -- mechanism proven,
 * guard proven sufficient -- or does not, and kills this hypothesis outright.
 * Shipping it ON on the argument above is the mistake RECOMP_APU_REON_HEAD_NOP
 * was already made with, twenty lines further down this file.
 *
 * Cost if it is switched on and the hypothesis is right: nothing. VOICE_ON and
 * VOICE_RELEASE drop the lock at the end of their handlers, so a voice that is
 * genuinely idle traps on the next subframe, 1/1500 s later. Cost if a guest
 * VOICE_LOCK is never released: that voice never retires -- which is why
 * locked_raises is reported rather than silently swallowed.
 *
 * RECOMP_APU_IDLE_TRAP_LOCK_GUARD=1 enables. */
/* DO NOT RAISE THE IDLE TRAP FOR A VOICE THAT IS IN NO LIST.
 *
 * RECOMP_APU_IDLE_TRAP_SELFLINK, default OFF pending an A/B.
 *
 * The idle trap means one thing: this voice is LINKED INTO A LIST and is not
 * active, so tell DirectSound to take it out. `link(v) = v` is the driver's
 * own marker for "not in any list" -- CMcpxCore::SetupVoiceProcessor writes it
 * for all 256 voices at boot and RemoveIdleVoice writes it for every voice it
 * removes. A self-linked voice therefore fails the trap's own precondition,
 * and raising for it asks the guest to remove something already removed.
 *
 * WHY IT STILL HAPPENS, and it is the HEAD that does it. RECOMP_APU_SELFLINK_END
 * already honours the marker, but only for the NEXT pointer: it rewrites nxt to
 * 0xFFFF so the walk stops after v. The voice itself is still processed, and if
 * it is inactive the trap is raised. For a voice sitting at a list head that is
 * every subframe, for ever, because nothing downstream can move a head.
 *
 * MEASURED, player session 17 Sep 2026 12:17. v1 retired, went idle, never came
 * back on, and its antecedent read TVL -- the top of the 3D voice list -- in
 * 835 of the ring's recorded raises. It collected 13,651 raises against v3's
 * 173 and every other voice's three or fewer, while selflink_end terminated
 * 364,626 walks. The guest acknowledged each raise through the main APU
 * registers and never updated the head.
 *
 * SUPPRESSES THE RAISE ONLY, through the same `suppress` path the lock guard
 * uses, so the cursors still advance and an ACTIVE self-linked voice is still
 * rendered. That last part is load-bearing: VOICE_ON prepends a voice that
 * regs[top] already names, so link(selected) = selected, and a legitimate
 * single-voice list is self-linked AND active. Skipping the walk rather than
 * the raise would silence it. */
int mcpx_apu_idle_trap_selflink(void)
{
    static int on = -1;
    if (on < 0) on = recomp_switch_on("RECOMP_APU_IDLE_TRAP_SELFLINK");
    return on;
}

int mcpx_apu_idle_trap_lock_guard(void)
{
    static int on = -1;
    if (on < 0) {
        /* REVERTED TO OFF, 16 Sep 2026, SAME DAY IT WAS TURNED ON, because
         * turning it on made the crash it was meant to prevent BOTH MORE
         * FREQUENT AND DETERMINISTIC.
         *
         *     guards off   ~5 faults in ~25 runs, at t=34.05 to t=35.03
         *     guards on    4 of 4 runs, every one at t=24.03
         *
         * Identical to two decimal places across runs, where the original
         * fault was probabilistic and 10 s later. That is not the same crash
         * arriving sooner; it is a new one.
         *
         * The mechanism is not established, and the likeliest reading is the
         * obvious one: withholding the raise for a locked voice means that
         * voice is never retired, so whatever waits on the retirement waits
         * for ever. The guard advances the cursors so it cannot pin the WALK,
         * but nothing was checked about what happens to the voice.
         *
         * THE MEASUREMENT THAT MOTIVATED IT WAS NOT WHAT IT SAID IT WAS.
         * "13,294 of 23,933 raises happen while the guest holds that voice's
         * lock" is two counters with different denominators divided into each
         * other -- see the correction further down. The WINDOW is real and was
         * established by reading VOICE_ON, not by that ratio. What was never
         * established is either its size or that suppressing the raise is a
         * safe way to close it. A fix that makes the crash worse is not a fix,
         * however good the reasoning behind it was.
         *
         * Kept as a switch because the counters behind it are what found the
         * window, and because the next attempt needs this arm to compare
         * against. Do not default it on again without a run count: the fault
         * fires in about one run in five with it off, so five clean runs is
         * the minimum evidence, not one.
         *
         * The original note follows.
         *
         * DEFAULT ON since 16 Sep 2026, on a measurement that nearly went the
         * other way. The guard shipped off because its mechanism was static
         * reading and the ring was built to decide it in one run. That run was
         * taken -- and read too early. At t=86 s it showed raises=9, locked=0,
         * which looked like the hypothesis dying. Over the whole run:
         *
         *     raises=23933  locked=13294  never_on=0  repeat=23500
         *
         * CORRECTED 16 Sep 2026, AND THE CORRECTION MATTERS. That 13,294 was
         * not a subset of that 23,933. The `locked` counter lived outside the
         * raise gate, so it counted every locked ENCOUNTER during a walk --
         * 1500 Hz, per voice, per list -- while `raises` counted raises. The
         * "56%" derived from them is a ratio of two different populations and
         * is not a percentage of anything; in 42 of the 152 distinct report
         * lines in build-macos the "subset" is larger than the set it was
         * supposedly a subset of. The counter is now split into
         * locked_enc and locked_raises and the guard's justification has to be
         * re-derived from a run that prints the second.
         *
         * That mis-sized number is the best available explanation for why this
         * guard's effect was predicted so badly -- defaulting it on took the
         * fault from ~5 in 25 runs at t=34-35 s to 4 of 4 at t=24.03 s. Do not
         * re-enable it on the strength of anything above.
         *
         * The MECHANISM below is unaffected by the arithmetic error, and is
         * still the window: VOICE_ON publishes the handle to
         * the voice list about eighty lines before it sets ACTIVE_VOICE, on a
         * guest thread, and the walk runs on the frame thread. A walk landing
         * in that gap calls an inactive voice idle and hands DirectSound a
         * handle it has not finished giving an owner object.
         *
         * never_on=0 across the run is the other half of the answer: we never
         * reach a handle the guest has NEVER started, so the walk is not lost
         * -- it is early.
         *
         * READING A COUNTER EARLY AND CONCLUDING FROM IT is the mistake this
         * comment exists to stop the next person repeating; it is the same
         * mistake as quoting a frame time from a run still in progress, and
         * both were made in this session.
         *
         * =0 restores the window and keeps g_idle_trap_locked_raises counting
         * in both arms, so a run with the guard off still says whether it
         * would have mattered. */
        const char *e = getenv("RECOMP_APU_IDLE_TRAP_LOCK_GUARD");
        on = e ? (atoi(e) != 0) : 0;   /* REVERTED -- see below */
    }
    return on;
}

unsigned long g_idle_trap_lock_suppressed;
unsigned long g_apu_method_while_trapped;
unsigned long g_apu_fedec_held;

unsigned long g_apu_voice_on_count;
unsigned long g_apu_voice_off_count;
unsigned long g_apu_voice_off_already_count; /* retired a voice already inactive */
unsigned long g_apu_voice_release_count;
unsigned long g_apu_idle_trap_count;
unsigned long g_apu_voice_process_count;
/* Mixbin fate. See the block in voice_process that increments these. */
unsigned long g_bin_heard_2d, g_bin_lost_2d, g_bin_heard_3d, g_bin_lost_3d;
unsigned long g_bin_lost_hist[32];
/* Mirrored at the write site so the report can name the value that decides
 * every 3D voice's fate without needing the device pointer. */
unsigned char g_bin_submix[4]; int g_bin_hrtf_on;
int mcpx_apu_mixdown_all(void); /* apu_dsp.c: which bins the mixdown sums */
/* Default 0: the override is NOT applied, because nothing downstream performs
 * HRTF or mixes its submixes down. =1 restores upstream's unconditional
 * override for an A/B. */
static int mcpx_apu_hrtf_bins(void)
{ static int on=-1;
  if(on<0){const char*e=getenv("RECOMP_APU_HRTF_BINS"); on = e ? (atoi(e)!=0) : 0;}
  return on; }

/* fe_methods includes internal SE2FE events. guest_methods counts only
 * arrivals through mcpx_apu_vp_write, independently of idle-trap arming.
 * SET_CURRENT_VOICE is a second positive control for guest submissions.
 * A zero delta means no methods arrived here; it cannot distinguish a guest
 * that stopped submitting from writes lost before reaching this entry point. */
unsigned long g_apu_fe_method_count;
unsigned long g_apu_guest_method_count;
unsigned long g_apu_voice_off_command_count;
unsigned long g_apu_set_current_voice_count;
unsigned long g_apu_voice_on_loop_count;

#define APU_UNKNOWN_METHOD_MAX 64
unsigned long g_apu_unknown_method_count;
uint32_t g_apu_unknown_method[APU_UNKNOWN_METHOD_MAX];
unsigned g_apu_unknown_method_n;

/* Where a voice's forward link comes from at VOICE_ON.
 *
 * SET_ANTECEDENT_VOICE is only ever issued by the guest -- the one internal
 * fe_method call is SE2FE_IDLE_VOICE -- so antecedent_sets is guest traffic by
 * construction, and comparing it against VOICE_ON tells a dropped store from a
 * guest that genuinely reuses a stale FEAV. The two branch counters say which
 * insert ran; self_ante is the INHERIT case where FEAV names the voice being
 * turned on, which collapses that branch's read-modify-write to link(v) = v.
 *
 * self_link is the OUTCOME, read back from the voice after the insert, and it
 * is the one to trust: it covers the TOP branch reaching the same state via a
 * list head that already names this voice, and it stands even if the reasoning
 * about the INHERIT arithmetic is wrong. A self-link is a one-entry cycle in
 * the list mcpx_apu_vp_frame walks. */
unsigned long g_apu_selflink_terminated;
/* Raises withheld because the voice's own link said it was in no list.
 * See mcpx_apu_idle_trap_selflink. */
unsigned long g_idle_trap_selflink_suppressed;
/* WAS THE GUEST EVER TOLD ABOUT THIS VOICE AT ALL?
 *
 * The trap carries ONE handle. fe_method writes FEDECMETH/FEDECPARAM, the
 * guest's ISR reads the pair, and the raise site's own comment says re-raising
 * "would overwrite FEDECPARAM -- the handle the guest has not read". So when a
 * burst retires several voices at once, only one handle can be outstanding,
 * and the coalescing deliberately withholds the rest.
 *
 * The question that follows, and which nothing in this tree could answer: of
 * the voices that went idle, how many ever had their handle DELIVERED? A voice
 * the guest was never told about cannot be removed by it, stays linked, and
 * contributes a permanent raise per subframe for the rest of the run. That is
 * the shape of the storm the player hears as "the sound breaks up when you
 * speak to gum" -- a burst of VOICE_ON, then a plateau that never recovers.
 *
 * `seen` is set wherever a voice is found inactive-and-linked, before any
 * suppression. `delivered` is set only when fe_method actually WROTE the pair:
 * with RECOMP_APU_FEDEC_HOLD on, a raise arriving at a trapped front end is
 * held and the handle never reaches the registers, so raising and delivering
 * are different events and counting the raise would overstate it.
 *
 * seen == delivered means every idle voice was named and the guest simply did
 * not act. seen > delivered names the voices it was never told about, and that
 * difference is the measurement. */
static uint64_t g_idle_seen_mask[(MCPX_HW_MAX_VOICES + 63) / 64];
static uint64_t g_idle_delivered_mask[(MCPX_HW_MAX_VOICES + 63) / 64];
unsigned long g_idle_seen_distinct, g_idle_delivered_distinct;
unsigned long g_idle_trap_selflink_encounters;
unsigned long g_apu_trap_suppressed;

int mcpx_apu_se_while_trapped(void);   /* apu_core.c */
void mcpx_apu_idle_trap_report(int crash); /* defined below, beside the ring */

/* On by default. Unlike the self-link guard this is not a new behaviour looking
 * for a justification -- it is the existing "do not overwrite a handle the
 * guest has not read" rule applied across frames instead of only within one
 * walk. */
int mcpx_apu_trap_coalesce(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("RECOMP_APU_TRAP_COALESCE");
        on = (e && *e) ? (atoi(e) != 0) : 1;
    }
    return on;
}

/* OFF by default. RECOMP_APU_IDLE_TRAP_EDGE=1 enables.
 *
 * WHAT IT CHANGES. SE2FE_IDLE_VOICE stops being raised for the level "this
 * voice is inactive and still in a list" and starts being raised for the edge
 * "this voice has just become inactive while in a list". A different voice
 * going idle still raises immediately; only a repeat for a voice we have
 * already reported and that has not been restarted since is withheld.
 *
 * WHY THAT IS THE HARDWARE BEHAVIOUR AND NOT A THROTTLE. The driver's ISR has
 * three early returns that all leave the voice inactive and still linked --
 * h >= 0x100, this->+0x2C0 set, and CFG_FMT PERSIST (001A241F). Against a
 * level-triggered trap every one of them is an infinite interrupt loop with
 * the driver behaving exactly as written. PERSIST in particular is a
 * documented DirectSound feature and this model's own stream path implements
 * it by returning without calling voice_off (see the `persist` branches in
 * voice_get_samples), i.e. by deliberately leaving a voice inactive and
 * linked. Hardware that re-raised on that state would make PERSIST unusable.
 * So the level reading cannot be right, and the edge reading is the only one
 * that makes the driver's own code sane.
 *
 * WHAT IT DOES NOT CLAIM. It is not proven to be why sound effects are silent.
 * The measured chain -- storm, front end TRAPPED on 91-92% of subframes, guest
 * write rate into the VP aperture collapsing to zero, [APU-VOICE] on= frozen
 * -- is consistent with the storm throttling the guest's sound engine, and
 * this removes the storm's engine. Whether removing it lets voices start again
 * is a run, not an argument, which is why this ships OFF and why
 * g_idle_trap_edge_encounters is counted with it off: one run with the switch
 * OFF says how many raises it would have withheld, and if that is not most of
 * them the hypothesis is dead before anybody enables it.
 *
 * THE RISK IT CARRIES, stated plainly: if the guest ever genuinely needs a
 * second notification for a voice it failed to service the first time, this
 * withholds it and that voice stays in the list inactive for ever. Nothing
 * here can rule that out statically. g_idle_trap_edge_rearm is the counter
 * that would show the latch cycling normally rather than latching shut. */
int mcpx_apu_idle_trap_edge(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("RECOMP_APU_IDLE_TRAP_EDGE");
        on = e ? (atoi(e) != 0) : 0;
    }
    return on;
}

/* RECOMP_APU_IDLE_TRAP_REARM_MS, default 16, only meaningful with the edge on.
 *
 * WHY NEITHER ARM WAS RIGHT. The level trap tells the guest about an
 * inactive-and-linked voice 1500 times a second and the music breaks up under
 * the interrupt load. The edge trap tells it exactly once -- and the guest's
 * ISR has three early returns, so if that once lands while it is not ready
 * the voice is never reclaimed. Player session 16 Sep 2026 16:22, edge on:
 * 2D heard froze at 29597 twenty seconds in, edge_suppressed=1010411,
 * rearm=51, off == off_commands == 94. The guest stopped its own voices,
 * was told once, did not reclaim them, ran out of free voices, and stole
 * playing ones: effects cut off, then a music voice went.
 *
 * The model cannot know when the guest has FINISHED with a notification, so
 * the honest shape is: report the transition, and if the voice is still
 * sitting there after one guest tick, report it again. 16 ms is one
 * DirectSound tick; at 1500 subframes/s that is 24 withheld subframes per
 * re-raise, 60 raises/s per stuck voice instead of 1500. =0 restores the
 * pure edge for an A/B. */
static unsigned mcpx_apu_idle_trap_rearm_subframes(void)
{
    static int sf = -1;
    if (sf < 0) {
        const char *e = getenv("RECOMP_APU_IDLE_TRAP_REARM_MS");
        int ms = e ? atoi(e) : 16;
        sf = ms > 0 ? (ms * 3 + 1) / 2 : 0;
        if (sf > 65535) sf = 65535;   /* hold[] is uint16_t; past this the
                                       * latch would silently never re-raise */
    }
    return (unsigned)sf;
}

/* OFF by default, and UNMEASURED. Read that as written: not "measured and found
 * useless", which is what this comment used to say.
 *
 * It said the guard "fired 47,293 times and left `trapped` at 50.3%,
 * indistinguishable from the runs without it". Both halves of that fail:
 *
 *   * there were no runs without it. The control arm set
 *     RECOMP_APU_SELFLINK_END=0, and until the same day this function tested
 *     for the variable's PRESENCE, so the control had the guard on too. See
 *     the note on mcpx_apu_selflink_end below.
 *   * three of that A/B's four runs never reached voice churn at all --
 *     on=5 off=0 idle_trap=0, the parked-player signature, and 12 of 199 pad
 *     events fired. One run in four had any churn and it was in the guard=1
 *     arm, so 50.3% had nothing to be indistinguishable from.
 *
 * What the switch IS remains what it was: link(v) = v is the driver's "not in
 * any list" marker, written by SetupVoiceProcessor for all 256 voices at boot
 * and by RemoveIdleVoice for every voice it removes, so treating it as
 * end-of-list honours a convention the guest already relies on. An unbounded
 * self-cycle is also a defect on its own terms -- the walk's only protection is
 * a 256-iteration cap, so a self-linked ACTIVE voice would be rendered 256
 * times in a subframe.
 *
 * AND THERE IS NOW AN ARGUMENT AGAINST IT, which is why the default does not
 * move on the strength of the paragraph above. The case for "terminating cannot
 * lose a reachable voice" rested on the guest having already written
 * link(v) = v before the re-ON. Measured across four instrumented runs, that is
 * false in every one of the sixteen cycle-forming inserts: link(v) held a real
 * successor, and OUR insert overwrote it. Terminating therefore drops whatever
 * the guest still had behind v. So does looping, so the guard is not worse --
 * but neither is lossless, and that points back at the insert.
 * See docs/jsrf/progress/CLAUDE_PROGRESS_2026-09-15_THE_HEAD_WAS_NEVER_STALE.md.
 *
 * MEASURED PROPERLY, 15 Sep 2026, and it stays off. Six boots on
 * pad/gameplay_nobarrage.pad with the switch actually working, five reaching a
 * scene-verified mission, two arms that report different states:
 *
 *                      guard OFF (n=2)      guard ON (n=3)
 *   idle traps raised  19706  18891         18957  18883  18344
 *   halted frames       6350   5809          2286   1564   1457
 *   engine duty        98.43% 98.57%        99.44% 99.61% 99.64%
 *   longest trap run      47     47            87     55     55  frames
 *   frame time         36.74  31.95 ms      36.44  35.31  34.07 ms
 *
 * THE TRAP STORM IS UNCHANGED. Raises overlap between the arms and so does the
 * busiest voice's share of them, which swings 70-98% inside a single arm. So
 * the verdict this comment used to assert without evidence is now the verdict
 * with evidence: terminating the cycle does not stop the storm. The reason is
 * the one already written down -- the self-linked voice is still the head of
 * its list and still inactive, so the walk begins on a dead voice and traps
 * whether or not it then loops.
 *
 * TWO THINGS DID MOVE, IN OPPOSITE DIRECTIONS, AND NEITHER DECIDES IT.
 * Engine duty is higher with the guard on and the ranges do not overlap --
 * 98.5% against 99.6%, which is dropped subframes falling from ~6100 to ~1700
 * a run -- and that is entirely the halted-frame count, a third of what it is
 * with the guard off. But `halted` is the guest writing FECTL, so the path from
 * this switch to that count is not established and could be scene variation at
 * n=2. Against it, the guest takes LONGER to service each trap: the longest
 * trapped run goes from 31 ms to 37-58 ms. Frame time does not separate.
 *
 * A one-point duty gain of unexplained provenance does not outweigh the tail
 * loss above, so the default does not move. What would decide it is the
 * mechanism behind `halted`, not more runs of this A/B.
 * diagnostics/jsrf_first_fault/ab_switch.sh takes it. */
/* THE SWITCH TESTED FOR THE VARIABLE'S PRESENCE, SO `=0` TURNED IT ON.
 *
 * `getenv(...) != NULL` is the right shape for a trace, where setting the name
 * at all means "start printing", and it is what most RECOMP_* switches here
 * do. It is the wrong shape for an A/B switch, and this one's own comment two
 * paragraphs up promised the opposite: "RECOMP_APU_SELFLINK_END=0 restores the
 * previous behaviour for A/B".
 *
 * So the only A/B ever taken of it ran with the guard ON IN BOTH ARMS -- the
 * control arm set the variable to 0 and thereby enabled the very thing it was
 * controlling for. Both arms' reports even said "(guard on)"; nobody read them.
 * That is the second independent reason the "it moves nothing" verdict in
 * CLAUDE_HANDOVER_2026-09-15 is unsupported, and it is the mechanical one: the
 * first is that three of that A/B's four runs never reached voice churn.
 *
 * Value-tested now, matching mcpx_apu_trap_coalesce and
 * mcpx_apu_se_while_trapped, which were written correctly. The other switches
 * in this tree documented as `=0`-disableable -- RECOMP_GPU_OWN and
 * RECOMP_PHYSICAL_HEAP_ALIAS -- were checked the same day and honour it. */
int mcpx_apu_selflink_end(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("RECOMP_APU_SELFLINK_END");
        on = e ? (atoi(e) != 0) : 0;
    }
    return on;
}

/* OFF by default, pending an A/B, and the story of that default is the useful
 * part of this comment.
 *
 * WHAT IT DOES. VOICE_ON for a voice that is already its list's head stores
 * that voice's own handle over its successor, so everything behind it in the
 * list becomes unreachable -- not unrendered, GONE, because the successor
 * handle lived only in the field just overwritten. The walk then spins on the
 * one-entry cycle for its full 256-iteration cap every subframe.
 * RECOMP_APU_SELFLINK_END terminates that cycle but cannot undo it, which is
 * why that switch measured as no help and this is a different question.
 *
 * WHY THE ARGUMENT IS GOOD. Prepending a voice that is already the head is a
 * no-op on a list; doing it as a write is data loss. It does not condition away
 * the TOP write that CMcpxVoiceClient's debug validation depends on -- in this
 * branch regs[top] already IS the handle, so dwTVL still names the head. It
 * fired 40 times in a 280 s scripted run, so the state is common.
 *
 * AND WHY THAT IS NOT ENOUGH. The first verification run, against two pre-fix
 * runs on the same schedule, went the wrong way on two counters:
 *
 *                    on        idle_trap        processed
 *     before     250, 290    19706, 18891    3.73M, 5.46M
 *     after           284           30709           1.83M
 *
 * n=1 against n=2 decides nothing -- ab_switch.sh refuses exactly this
 * comparison -- but it is not the result a working fix owes you either, and it
 * was taken on a scripted schedule whose voice traffic differs run to run.
 * This switch was briefly default-ON on the strength of the paragraph above
 * plus a player's silent session. That is this repository's own rule broken:
 * do not ship on an argument before a run says which value is wrong.
 *
 * RECOMP_APU_REON_HEAD_NOP=1 enables it. */
unsigned long g_apu_voice_on_head_nop;

int mcpx_apu_reon_head_nop(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("RECOMP_APU_REON_HEAD_NOP");
        on = e ? (atoi(e) != 0) : 0;
    }
    return on;
}

/* THE INSERT, RATHER THAN THE WALK THAT TRIPS OVER IT.
 *
 * VOICE_ON's TOP branch does, unconditionally:
 *
 *     link(v) = regs[top];   regs[top] = v;
 *
 * which is right for a voice that is not in the list and catastrophic for one
 * that is. link(v) is the ONLY pointer to v's successor -- the voice register
 * file lives in guest RAM and this model keeps no shadow of it -- so the first
 * store destroys the tail and the second splices v in front of a list that
 * still reaches v. The result is a ring, and the walk then renders every voice
 * inside it once per lap, for ever.
 *
 * WHAT IT COSTS, from counters that were already in the logs -- voice_process
 * calls per subframe, i.e. `processed` differenced against `se` between two
 * consecutive report lines. voice_process is called once per VISIT with no
 * dedup, bounded only by the walk's 256-iteration cap:
 *
 *                clean windows      ringing windows
 *   apuclock2      4.00-5.24        12.2 ->  77.1
 *   idxfix3        5.00             10.9 ->  71.4
 *   reverted       4.73             13.2 -> 194.6
 *
 * That is audible rather than merely expensive: each visit advances cbo by
 * another 32 samples, steps the amplitude and filter envelopes again, and ADDS
 * its different 32 samples into the same bin, which the EP then hard-clamps
 * (apu_dsp.c:151-157). Voice 0's 14,016-sample ADPCM loop at N=70 finishes in
 * ~4 ms instead of 292 ms -- a ~240 Hz buzz where the music should be. The
 * other failure mode is silence: `reverted` also has three consecutive 10 s
 * windows at 0.00 voice_process calls, the ring having closed around members
 * that are all inactive. One run holds both, minutes apart.
 *
 * WHAT THIS SWITCH DOES. Before the splice, walk from regs[top]; if v is
 * already reachable, take it out of the list first, then prepend as before.
 * That is a move-to-front, which is what "insert at the head a voice that is
 * already in this list" means on a list. Two populations, and the counters
 * printed on the [APU-REON] line separate them because their histories differ:
 *
 *   - v IS ALREADY THE HEAD. Unlink-then-prepend would write back the two
 *     values it just read, so the insert is a no-op and this does literally
 *     nothing -- which is RECOMP_APU_REON_HEAD_NOP, reached by another route
 *     and arriving at the same final state. This switch therefore SUBSUMES
 *     that one exactly; turn this on and the head guard has nothing left to do.
 *   - v IS DEEPER. link(pred) = link(v) detaches v and leaves its successor
 *     hanging off its old predecessor; the prepend then moves v to the head.
 *     Nothing becomes unreachable. This is the case nothing in this file has
 *     ever repaired and the one that forms rings of length >= 2.
 *
 * HOW THE TWO SPLIT. Every completed run in this tree that reached voice churn
 * and carries the cycle counters -- ten of them, all with reon_head_nop OFF, so
 * link_after == v in the TOP branch happens iff regs[top] == v and self_link IS
 * the head population -- read as self_link/relink:
 *
 *   reverted   356/370    e32_1     174/195    caponly1  97/117
 *   apuclock2  173/209    capseq1   165/210    idxfix3   87/114
 *   capseq2    107/141    idxsplit1 122/193    idxfix2   60/93
 *   idxsplit2   44/57
 *
 * 63.2% to 96.2% head, so the deep case is 4% to 37% of re-inserts and runs
 * from 13 to 71 of them per session -- not the 17% a single run suggests.
 * Both halves are worth having, and that is the point: each of the two existing
 * guards addresses one half and misses the other, which is why each measured as
 * nothing on its own.
 *
 * WHAT IT DOES NOT FIX, and this is the paragraph to read before calling a run
 * with walks_with_a_cycle != 0 a failed fix. The guest writes link(v) = v
 * ITSELF, at runtime, to voices our TVL still names, and in the head case this
 * switch deliberately writes nothing -- so a guest-set sentinel survives it and
 * the walk still meets a one-entry ring. The measurement is the three runs
 * taken with the head guard on, where our code cannot have written the
 * sentinel and self_link therefore counts only the guest's:
 *
 *   ab-reon-t1_reon1   head_nop=104   self_link=99
 *   ab-reon-t2_reon1   head_nop=47    self_link=46
 *   reon-verify        head_nop=40    self_link=8
 *
 * 99 of 104 and 46 of 47 head-case re-inserts found link(v) already equal to v.
 * No insert-side change can prevent that. The read-side half is
 * RECOMP_APU_SELFLINK_END, which is why the two are meant to be measured
 * together and why this one on its own should not be expected to take
 * walks_with_a_cycle to zero.
 *
 * THE RACE IS REAL AND THIS DOES NOT CLOSE IT. VOICE_ON runs on a guest thread;
 * voice_lock() takes d->lock and releases it immediately, while the frame
 * thread holds it across the whole of se_frame. So the unlink's store and the
 * prepend's two stores race mcpx_apu_vp_frame's walk exactly as the original
 * two stores did. What changes is WHICH torn states are reachable: a walk that
 * lands mid-unlink now sees a list SHORT by one voice for the width of one
 * insert -- one voice unrendered for at most a 1/1500 s subframe -- where
 * before it could see a list that was circular for the rest of the run. That is
 * the trade, stated rather than hidden. It is not a claim that the insert is
 * atomic. The real fix is the lock discipline, and it is not this.
 *
 * OFF by default. This repository shipped one APU guard on the strength of a
 * good argument and made the crash worse, and the rule that came out of that is
 * a run count, not a better argument: five clean runs per arm before the
 * default moves, where "clean" is defined by the positive control on the
 * [APU-CYCLE] line. RECOMP_APU_LIST_MOVE_TO_FRONT=1 enables it. */
unsigned long g_apu_list_mtf_head;     /* re-ON of a voice already at the head */
unsigned long g_apu_list_mtf_deep;     /* ...already deeper in: the ring case */
unsigned long g_apu_list_mtf_inherit;  /* ...unlinked from the INHERIT branch */

int mcpx_apu_list_move_to_front(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("RECOMP_APU_LIST_MOVE_TO_FRONT");
        on = e ? (atoi(e) != 0) : 0;
    }
    return on;
}

unsigned long g_apu_antecedent_set_count;
unsigned long g_apu_voice_on_top_count;
unsigned long g_apu_voice_on_inherit_count;
unsigned long g_apu_voice_on_self_ante_count;
unsigned long g_apu_voice_on_self_link_count;

/* CYCLES OF LENGTH >= 2, WHICH NOTHING HERE HAS EVER LOOKED FOR.
 *
 * g_apu_voice_on_self_link_count catches link(v)==v, the one-entry cycle whose
 * consequences are written up at length above the REON_HEAD_NOP guard. The
 * general case is the same failure one notch out and has no instrument at all:
 * VOICE_ON for a handle that is already somewhere in the list -- not at the
 * head, so the head_nop guard does not apply -- takes the TOP-insert branch,
 * writes link(v) = regs[top], and splices v in front of a list that still
 * reaches v. The result is a ring, and everything behind the splice point is
 * unreachable.
 *
 * Three things follow, and only the first is a waste of CPU:
 *   - the walk spins to its 256-iteration cap every subframe. The cap's break
 *     logs through DPRINTF, which compiles to nothing, so it is silent.
 *   - voices inside the ring are rendered MORE THAN ONCE per subframe, each
 *     pass advancing cbo and ssl_seg again. A streaming voice drains at 2x and
 *     starves; a looping voice plays at 2x pitch. That is an audio-sync
 *     mechanism independent of the output clock.
 *   - our TVL keeps naming a ring member for ever. The guest's RemoveIdleVoice
 *     repairs ITS list and clears the owner, but cannot repair ours, so every
 *     later walk finds the voice inactive and still reachable and raises
 *     SE2FE_IDLE_VOICE for a handle whose owner is NULL -- which is
 *     sub_001A2E2E with ecx=0, the top crash site, arrived at without either
 *     of the two windows the existing guards were built for. A THIRD
 *     MECHANISM, and the one that fits the fatal ring entry carrying no flags.
 *
 * COUNTERS ONLY. The walk still breaks where it always did. cycle_break is the
 * behaviour change and it is OFF: this repository has already shipped one APU
 * guard on an argument and made the crash worse, and the rule that came out of
 * that is a run count, not a better argument. What these answer first is
 * whether the rings exist at all -- and the existing idle-ring transcripts say
 * they do, e.g. `2D:v68[]<-v69` followed by `2D:v69[]<-v68`.
 *
 * Positive control for relink: g_apu_voice_on_top_count, which is nonzero in
 * every run. relink must never exceed it, and must be >= self_link. */
unsigned long g_apu_voice_on_relink_count;   /* VOICE_ON for a handle already in the list */
unsigned long g_apu_list_cycles_seen;        /* walks that revisited a voice */
unsigned long g_apu_list_walk_cap;           /* ...and ran out of iterations */
unsigned long g_apu_list_cycle_breaks;       /* ...and stopped early because of it */
unsigned      g_apu_list_cycle_last;         /* the voice that closed the last one */
unsigned      g_apu_list_cycle_list;
static uint64_t g_apu_voice_in_cycle[4];     /* per-voice sticky bit, for the ring flag */

int mcpx_apu_cycle_break(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("RECOMP_APU_CYCLE_BREAK");
        on = e ? (atoi(e) != 0) : 0;
    }
    return on;
}

/* Is `h` reachable from the head of `top_reg`'s list? Bounded by the voice
 * count, so it terminates on a list that is already a ring. */
static int voice_list_contains(MCPXAPUState *d, unsigned top_reg, uint16_t h)
{
    uint16_t cur = (uint16_t)d->regs[top_reg];
    int i;
    for (i = 0; i < MCPX_HW_MAX_VOICES && cur != 0xFFFF; i++) {
        if (cur >= MCPX_HW_MAX_VOICES) break;
        if (cur == h) return 1;
        cur = (uint16_t)voice_get_mask(d, cur, NV_PAVS_VOICE_TAR_PITCH_LINK,
                                       NV_PAVS_VOICE_TAR_PITCH_LINK_NEXT_VOICE_HANDLE);
    }
    return 0;
}

/* WHERE does `h` hang off `top_reg`'s list, not merely whether.
 *
 * The bool above is enough to COUNT a re-insert and useless for repairing one:
 * to take v out of a list you need the field that points AT it, and that field
 * is either the head register or some other voice's PITCH_LINK. So this is the
 * same walk with the cursor's predecessor carried along.
 *
 * Returns 1 when h is reachable, writing the predecessor's handle into *pred --
 * 0xFFFF meaning "h is the head, and the pointer at it is d->regs[top_reg]
 * itself", which is the same sentinel the frame walk's `came_from` uses for the
 * same thing. Returns 0 otherwise, with *pred left at 0xFFFF.
 *
 * THE THREE EXITS ARE THE FRAME WALK'S THREE EXITS and must stay that way: the
 * 0xFFFF terminator, a handle >= MCPX_HW_MAX_VOICES (NEXT_VOICE_HANDLE is a
 * full 16-bit field, so every value from 0x0100 to 0xFFFE is admissible and
 * invalid), and the MCPX_HW_MAX_VOICES iteration cap. The cap is not defensive
 * padding here -- this function is called specifically on lists that may
 * ALREADY be rings, which is the entire reason it exists, and without it the
 * repair would hang where the bug only degraded. An out-of-range h is never
 * found, because the range bail is tested before the comparison; callers guard
 * it anyway. */
static int voice_list_find_pred(MCPXAPUState *d, unsigned top_reg, uint16_t h,
                                uint16_t *pred)
{
    uint16_t prev = 0xFFFF;
    uint16_t cur = (uint16_t)d->regs[top_reg];
    int i;

    *pred = 0xFFFF;
    for (i = 0; i < MCPX_HW_MAX_VOICES && cur != 0xFFFF; i++) {
        if (cur >= MCPX_HW_MAX_VOICES) break;
        if (cur == h) {
            *pred = prev;
            return 1;
        }
        prev = cur;
        cur = (uint16_t)voice_get_mask(d, cur, NV_PAVS_VOICE_TAR_PITCH_LINK,
                                       NV_PAVS_VOICE_TAR_PITCH_LINK_NEXT_VOICE_HANDLE);
    }
    return 0;
}

/* Take `h` out of `top_reg`'s list, given the predecessor voice_list_find_pred
 * found for it. pred == 0xFFFF means h is the head and the pointer to repair is
 * the head register rather than another voice's link.
 *
 * link(h) IS DELIBERATELY LEFT ALONE. Every caller is about to overwrite it as
 * part of re-inserting h, so clearing it here would be a third store to the
 * same guest-RAM dword for no gain -- and while it still holds h's old
 * successor, a frame walk that reads between our two stores sees a list that is
 * merely missing an entry, not one that contradicts itself.
 *
 * THE link(h) == h CASE IS THE ONE THAT DECIDES THIS FUNCTION. That is the
 * driver's "not in any list" sentinel: CMcpxCore::SetupVoiceProcessor writes it
 * for all 256 voices at boot, RemoveIdleVoice writes it for every voice it
 * retires, and this model has five direct captures of the guest writing it at
 * runtime to a voice our TVL still names. Copy it into pred's link and pred
 * still names h -- the unlink silently does nothing, and the prepend that
 * follows closes exactly the ring this change exists to prevent. Reading it as
 * end-of-list instead loses nothing, by the same argument that justifies
 * RECOMP_APU_SELFLINK_END at the walk: if link(h) reads h, whatever was behind
 * h is already unreachable through h. It is applied here unconditionally, and
 * not behind that switch, because without it this is not an unlink.
 *
 * A link that is out of range rather than self or 0xFFFF is copied through
 * unchanged. The list then ends one entry earlier than it would have, at the
 * same bad handle and with the same [APU] out-of-range report from the frame
 * walk -- the walk was going to stop at h and read that handle regardless. */
static void voice_list_unlink_at(MCPXAPUState *d, unsigned top_reg,
                                 uint16_t pred, uint16_t h)
{
    uint16_t nxt = (uint16_t)voice_get_mask(
        d, h, NV_PAVS_VOICE_TAR_PITCH_LINK,
        NV_PAVS_VOICE_TAR_PITCH_LINK_NEXT_VOICE_HANDLE);

    if (nxt == h) nxt = 0xFFFF;

    if (pred == 0xFFFF) {
        d->regs[top_reg] = nxt;
        return;
    }
    voice_set_mask(d, pred, NV_PAVS_VOICE_TAR_PITCH_LINK,
                   NV_PAVS_VOICE_TAR_PITCH_LINK_NEXT_VOICE_HANDLE, nxt);
}

/* Defined with the decoder they belong to, further down the file; declared
 * here because the report is above it. */
extern unsigned long g_apu_adpcm_ok;
extern unsigned long g_apu_adpcm_fail;
extern unsigned long g_apu_adpcm_short;
extern unsigned long g_apu_adpcm_oversize;
extern unsigned long g_apu_adpcm_silenced;
int mcpx_apu_adpcm_guard(void);
int mcpx_apu_segment_spb(void);  /* the streaming block stride; see the definition */

void mcpx_apu_voice_report(void)
{
    {   /* Mixbin fate, and the HRTF submix that decides it. */
        extern unsigned long g_bin_heard_2d, g_bin_lost_2d,
                             g_bin_heard_3d, g_bin_lost_3d;
        extern unsigned long g_bin_lost_hist[32];
        extern unsigned char g_bin_submix[4]; extern int g_bin_hrtf_on;
        unsigned b; char hist[192]; int n = 0;
        for (b = 0; b < 32; ++b)
            if (g_bin_lost_hist[b] && n < (int)sizeof hist - 16)
                n += snprintf(hist + n, sizeof hist - (size_t)n, " %u:%lu",
                              b, g_bin_lost_hist[b]);
        hist[n] = 0;
        fprintf(stderr,
            "  [APU-BIN] 2D heard=%lu lost=%lu | 3D heard=%lu lost=%lu"
            " | hrtf_submix=%u,%u,%u,%u hrtf=%s mixdown=%s | lost by bin:%s\n",
            g_bin_heard_2d, g_bin_lost_2d, g_bin_heard_3d, g_bin_lost_3d,
            g_bin_submix[0], g_bin_submix[1],
            g_bin_submix[2], g_bin_submix[3],
            g_bin_hrtf_on ? "on" : "OFF",
            mcpx_apu_mixdown_all() ? "all" : "0-1", n ? hist : " none");
    }
    fprintf(stderr, "  [APU-VOICE] on=%lu off=%lu release=%lu idle_trap=%lu"
            " processed=%lu"
            " fe_methods=%lu set_current_voice=%lu on_loop=%lu"
            " guest_methods=%lu off_commands=%lu\n",
            g_apu_voice_on_count, g_apu_voice_off_count,
            g_apu_voice_release_count,
            g_apu_idle_trap_count, g_apu_voice_process_count,
            g_apu_fe_method_count, g_apu_set_current_voice_count,
            g_apu_voice_on_loop_count, g_apu_guest_method_count,
            g_apu_voice_off_command_count);
    { extern void mcpx_apu_ien_report(void); mcpx_apu_ien_report(); }
    { extern void mcpx_apu_cursor_report(void); mcpx_apu_cursor_report(); }
    /* off_already: see voice_off. raises-vs-idle_trap: g_apu_idle_trap_count
     * only increments when FETFORCE1 has SE2FE_IDLE_VOICE armed, but fe_method
     * writes FEDECMETH/FEDECPARAM BEFORE that test -- so if the guest ever
     * clears FETFORCE1 the two diverge, and each unarmed raise still clobbers
     * the decode pair the guest's polling ISR reads. Equal in every run so far,
     * and nothing asserted it; printing the difference is what makes a
     * divergence visible the first time it happens. */
    {
        /* The names matter more than the counts: these are the voices the
         * guest was never told about, so it cannot have removed them. */
        unsigned k, listed = 0;
        /* delivered > seen is impossible if both hooks sit in the same walk,
         * so it is reported as the instrument fault it is rather than
         * subtracted into a nonsense unsigned. */
        fprintf(stderr, "  [APU-IDLE-DELIVERY] found idle=%lu distinct,"
                " handle delivered=%lu distinct, never told about=%s%lu:",
                g_idle_seen_distinct, g_idle_delivered_distinct,
                g_idle_delivered_distinct > g_idle_seen_distinct
                    ? "BROKEN INSTRUMENT, delivered exceeds found by " : "",
                g_idle_delivered_distinct > g_idle_seen_distinct
                    ? g_idle_delivered_distinct - g_idle_seen_distinct
                    : g_idle_seen_distinct - g_idle_delivered_distinct);
        for (k = 0; k < MCPX_HW_MAX_VOICES && listed < 16; k++)
            if ((g_idle_seen_mask[k >> 6] & (1ULL << (k & 63)))
                && !(g_idle_delivered_mask[k >> 6] & (1ULL << (k & 63)))) {
                fprintf(stderr, " v%u", k);
                ++listed;
            }
        if (!listed) fprintf(stderr, " none");
        fprintf(stderr, "\n");
    }
    fprintf(stderr, "  [APU-VOICE2] off_already=%lu raises_minus_idle_trap=%ld\n",
            g_apu_voice_off_already_count,
            (long)g_idle_trap_raises - (long)g_apu_idle_trap_count);
    {
        unsigned k;
        fprintf(stderr, "  [VOICE-RELINK] links changed behind the model: %lu"
            " (of %lu reads)\n", g_guest_link_writes, g_link_checks);
    fprintf(stderr, "  [APU-FE-UNKNOWN] dropped=%lu distinct=%u:",
                g_apu_unknown_method_count, g_apu_unknown_method_n);
        for (k = 0; k < g_apu_unknown_method_n; k++)
            fprintf(stderr, " %04X", g_apu_unknown_method[k]);
        fprintf(stderr, "\n");
    }
    /* idle_edge rides in these parentheses rather than on a line of its own
     * because ab_score.py harvests switch state with
     *   \[APU-(?:TRAP|SELFLINK|REON)\][^(]*\(([^)]*)\)
     * and nothing else. A new line would leave an A/B on this switch in the
     * "assumes the environment took" class that voided the SELFLINK_END A/B.
     * ab_score.py's SWITCH_TOKEN still wants
     *   "RECOMP_APU_IDLE_TRAP_EDGE": "idle_edge"
     * added to it before the VOID rule can check this switch specifically;
     * without that entry the harvest works and the check is simply skipped. */
    fprintf(stderr, "  [APU-TRAP] suppressed=%lu (coalesce %s,"
            " se_while_trapped %s, idle_edge %s, idle_selflink %s)"
            " selflink_raises_withheld=%lu of %lu\n",
            g_apu_trap_suppressed,
            mcpx_apu_trap_coalesce() ? "on" : "OFF",
            mcpx_apu_se_while_trapped() ? "on" : "OFF",
            mcpx_apu_idle_trap_edge() ? "on" : "OFF",
            mcpx_apu_idle_trap_selflink() ? "on" : "OFF",
            g_idle_trap_selflink_suppressed,
            g_idle_trap_selflink_encounters);
    /* THE TWO NUMBERS THAT SEPARATE THE TWO STORM HYPOTHESES.
     *
     * persist is raises for a voice whose CFG_FMT says the guest's ISR returns
     * without acting (001A241F). edge_would is raises that name a voice we had
     * already reported idle and that has not been restarted since. Read both
     * against raises= on the line above, which is their denominator and their
     * positive control.
     *
     *   persist ~= raises   -> the storm is the PERSIST loop: the guest is
     *                          being handed the RIGHT handle and declining it,
     *                          so RECOMP_APU_FEDEC_HOLD cannot help and
     *                          RECOMP_APU_IDLE_TRAP_EDGE is the fix.
     *   persist ~= 0 but edge_would ~= raises -> still a repeat storm, but for
     *                          voices the ISR should have retired: the handle
     *                          it acted on was wrong, which is FEDEC_HOLD's
     *                          hypothesis, and edge only masks it.
     *   both ~= 0           -> every raise is a distinct fresh retirement and
     *                          neither switch is the answer.
     *
     * rearm is the latch's own liveness: suppressed climbing with rearm stuck
     * at zero means the latch has shut rather than cycled, which is the way
     * this change fails. */
    fprintf(stderr, "  [APU-IDLE-EDGE] persist=%lu edge_would=%lu"
            " edge_suppressed=%lu rearm=%lu reraise=%lu (of %lu raises,"
            " rearm_subframes=%u)\n",
            g_idle_trap_persist_raises, g_idle_trap_edge_encounters,
            g_idle_trap_edge_suppressed, g_idle_trap_edge_rearm,
            g_idle_trap_edge_reraise, g_idle_trap_raises,
            mcpx_apu_idle_trap_rearm_subframes());
    {
        unsigned distinct = 0, b;
        for (b = 0; b < MCPX_HW_MAX_VOICES / 64; ++b) {
            uint64_t w = g_pool_on_window[b];          /* portable popcount:
                                                        * this file also builds
                                                        * under MSVC */
            while (w) { w &= w - 1; ++distinct; }
        }
        fprintf(stderr, "  [APU-POOL] distinct=%u this window | on_2d=%lu"
                " on_3d=%lu on_active=%lu reuse=%lu (cumulative)\n",
                distinct, g_pool_on_2d, g_pool_on_3d,
                g_pool_on_active, g_pool_reuse);
        memset(g_pool_on_window, 0, sizeof g_pool_on_window);
        memset(g_pool_off_window, 0, sizeof g_pool_off_window);
    }
    /* THE ADPCM DECODE, WHICH HAD NO INSTRUMENT AT ALL. ok is the positive
     * control for fail: both zero means no ADPCM voice played in this run and
     * says nothing about the decoder. oversize is the stack-overrun clamp and
     * is the one thing here that is repaired unconditionally. */
    {   /* G7, beside the failures it was raised to explain. calls= is the
         * positive control: oob=0 with calls=0 means no voice used the
         * scatter-gather path at all and says nothing about the table. */
        extern unsigned long g_apu_sge_oob, g_apu_sge_calls;
        fprintf(stderr, "  [APU-SGE] %lu translations, %lu with an implausible"
                " page-table entry%s\n",
                g_apu_sge_calls, g_apu_sge_oob,
                g_apu_sge_calls == 0
                    ? "   <- the SG path was never used this run"
                    : (g_apu_sge_oob
                        ? "   <- reading PAST the page table; see G7"
                        : " (the table bound is holding)"));
    }
    fprintf(stderr, "  [APU-ADPCM] ok=%lu fail=%lu short=%lu oversize=%lu"
            " silenced=%lu (adpcm_guard %s, hw_header %s, %lu block(s)"
            " accepted that the strict header test refuses)\n",
            g_apu_adpcm_ok, g_apu_adpcm_fail, g_apu_adpcm_short,
            g_apu_adpcm_oversize, g_apu_adpcm_silenced,
            mcpx_apu_adpcm_guard() ? "on" : "OFF",
            g_adpcm_hw_header ? "on" : "OFF",
            g_adpcm_hw_header_accepted);
    {
        extern unsigned long g_adpcm_fail_ring, g_adpcm_fail_first, g_adpcm_fail_last, g_adpcm_fail_mid;
        extern uint16_t g_adpcm_fail_v[8]; extern uint8_t g_adpcm_fail_stream[8], g_adpcm_fail_ch[8];
        extern uint32_t g_adpcm_fail_block[8], g_adpcm_fail_nblocks[8], g_adpcm_fail_hdr[8];
        extern uint32_t g_adpcm_fail_page[8], g_adpcm_fail_prd[8], g_adpcm_fail_lin[8];
        extern uint32_t g_adpcm_fail_bs[8], g_adpcm_fail_ba[8], g_adpcm_fail_ebo[8];
        {
            extern unsigned long g_adpcm_spb_voice, g_adpcm_spb_seg, g_adpcm_spb_differ;
            if (g_adpcm_spb_voice)
                fprintf(stderr, "  [APU-ADPCM-SPB] stream segments=%lu,"
                        " segment spb used=%lu, disagreed with the voice"
                        " register=%lu (segment_spb %s)\n",
                        g_adpcm_spb_voice, g_adpcm_spb_seg, g_adpcm_spb_differ,
                        mcpx_apu_segment_spb() ? "on" : "OFF");
        }
        if (g_adpcm_fail_ring) {
            unsigned n = g_adpcm_fail_ring < 8 ? (unsigned)g_adpcm_fail_ring : 8u, i;
            fprintf(stderr, "  [APU-ADPCM-FAIL] first=%lu last=%lu mid=%lu | last %u:",
                    g_adpcm_fail_first, g_adpcm_fail_last, g_adpcm_fail_mid, n);
            for (i = 0; i < n; ++i) {
                unsigned slot = (unsigned)((g_adpcm_fail_ring - n + i) & 7u);
                fprintf(stderr, " v%u%s%s blk %u/%u pg%u prd=%08X lin=%08X hdr %08X"
                        " bs=%u ba=%u ebo=%u ba_blocks=%u",
                        g_adpcm_fail_v[slot],
                        g_adpcm_fail_stream[slot] ? "S" : "", g_adpcm_fail_ch[slot] == 2 ? "st" : "",
                        g_adpcm_fail_block[slot], g_adpcm_fail_nblocks[slot],
                        g_adpcm_fail_page[slot], g_adpcm_fail_prd[slot],
                        g_adpcm_fail_lin[slot], g_adpcm_fail_hdr[slot],
                        g_adpcm_fail_bs[slot], g_adpcm_fail_ba[slot],
                        g_adpcm_fail_ebo[slot],
                        g_adpcm_fail_bs[slot]
                            ? g_adpcm_fail_ba[slot] / g_adpcm_fail_bs[slot] : 0u);
            }
            fprintf(stderr, "\n");
            {
                extern unsigned long g_adpcm_fail_page0, g_adpcm_fail_pagehi;
                extern uint32_t g_adpcm_first_fail_blk[];
                extern uint32_t g_adpcm_fail_nblocks_v[];
                unsigned vv, shown = 0, failing = 0;
                /* THE GAP IS THE FINDING, so the line computes it.
                 *
                 * Measured on the 19 Sep 2026 black-screen run, by hand, off
                 * a log that did not print the buffer size: every voice
                 * starts failing EXACTLY 11 blocks before the end of its
                 * buffer -- v0 208/219, v1 126/137, v2 182/193, v3 190/201,
                 * v5 159/170, v6 181/192, v7 144/155. Seven voices, buffers
                 * from 137 to 219 blocks, the same 11 every time.
                 *
                 * A constant DIFFERENCE is not a stride error, which would
                 * leave a constant ratio, and 11 is not a page of blocks
                 * (113) -- so it is neither of the two theories this defect
                 * has already buried. It is a fixed over-read: the fetch
                 * runs off the end of the data by a constant amount, which
                 * is why block 0 always decodes and only the tail turns into
                 * 0x08080808.
                 *
                 * None of that was visible, because the line printed the
                 * failing block and not the buffer it was in. It prints both
                 * now, and the gap, so the next run reads the constant off
                 * the log instead of rediscovering it. */
                for (vv = 0; vv < MCPX_HW_MAX_VOICES; ++vv)
                    if (g_adpcm_first_fail_blk[vv]) ++failing;
                fprintf(stderr, "  [APU-ADPCM-PAGE] page0=%lu pagehi=%lu |"
                        " lowest failing block per voice:",
                        g_adpcm_fail_page0, g_adpcm_fail_pagehi);
                for (vv = 0; vv < MCPX_HW_MAX_VOICES && shown < 24; ++vv)
                    if (g_adpcm_first_fail_blk[vv]) {
                        uint32_t nb = g_adpcm_fail_nblocks_v[vv];
                        fprintf(stderr, " v%u:%u/%u", vv,
                                g_adpcm_first_fail_blk[vv], nb);
                        if (nb > g_adpcm_first_fail_blk[vv])
                            fprintf(stderr, "(gap%u)",
                                    nb - g_adpcm_first_fail_blk[vv]);
                        ++shown;
                    }
                /* Same trap as the VOICE-RATE table: this lists the eight
                 * LOWEST-numbered failing voices, so a truncated list reads
                 * exactly like a short one. Say how many were left out. */
                if (failing > shown)
                    fprintf(stderr, " ...and %u more failing voice(s) not shown",
                            failing - shown);
                fprintf(stderr, " (113 = 4096/36, one page of blocks)\n");
            }
        }
    }
    { extern void mcpx_apu_adpcm_extent_report(void);
      mcpx_apu_adpcm_extent_report(); }
    {
        /* THE SCORING LINE FOR "DO SOUND EFFECTS START", WITH ITS CONTROL ON
         * THE SAME LINE.
         *
         * on= alone cannot answer it. It is the symptom -- it climbs to 55 and
         * then freezes -- and it is also the scene marker (4-12 parked, 148-453
         * gameplay), so a frozen 55 is both "SE never start" and "we are
         * nowhere near either band" and nothing distinguishes them.
         *
         * Two separations, both on this line:
         *
         *   oneshot vs loop. A looping voice takes cbo = lbo at ebo and runs
         *   for ever by design, so BGM never retires; one-shots are what
         *   sound effects are. g_apu_voice_on_loop_count is already split out
         *   at VOICE_ON for exactly this reason. Music is known to work in
         *   this tree, so loop climbing is the control that says the guest's
         *   audio path is alive: oneshot=+0 beside loop=+N is SE silence.
         *
         *   DELTAS, not totals. A frozen total had to be found by differencing
         *   two reports ten seconds apart, which is how "on= is stuck" went
         *   unnoticed for three sessions. +0 in this window is one glance.
         *
         * guest_methods, raises and processed are the instrument-alive
         * controls: all three climbing beside oneshot=+0 is the signature the
         * established finding describes, and all three at +0 means the APU
         * stopped rather than the effects.
         *
         * Statics are safe here: this function has one caller, the periodic
         * reporter thread in diagnostics/jsrf_first_fault/main.c. The crash
         * handler calls mcpx_apu_idle_trap_report, not this. */
        static unsigned long p_on, p_loop, p_gm, p_raise, p_proc;
        long d_on    = (long)(g_apu_voice_on_count      - p_on);
        long d_loop  = (long)(g_apu_voice_on_loop_count - p_loop);
        long d_gm    = (long)(g_apu_guest_method_count  - p_gm);
        long d_raise = (long)(g_idle_trap_raises        - p_raise);
        long d_proc  = (long)(g_apu_voice_process_count - p_proc);
        long oneshot = (long)g_apu_voice_on_count
                     - (long)g_apu_voice_on_loop_count;
        long d_oneshot = d_on - d_loop;
        fprintf(stderr,
                "  [APU-SE] oneshot=%ld loop=%lu | this window:"
                " oneshot=%+ld loop=%+ld guest_methods=%+ld raises=%+ld"
                " processed=%+ld%s\n",
                oneshot, g_apu_voice_on_loop_count,
                d_oneshot, d_loop, d_gm, d_raise, d_proc,
                (d_oneshot == 0 && (d_proc > 0 || d_raise > 0 || d_gm > 0))
                    ? "   <- no one-shot voice started while the engine kept"
                      " running: SE silence, not a stopped APU"
                    : (d_proc == 0 && d_raise == 0 && d_gm == 0)
                        ? "   <- the APU saw nothing at all this window; this"
                          " line says nothing about SE"
                        : "");
        p_on = g_apu_voice_on_count;
        p_loop = g_apu_voice_on_loop_count;
        p_gm = g_apu_guest_method_count;
        p_raise = g_idle_trap_raises;
        p_proc = g_apu_voice_process_count;
    }
    fprintf(stderr, "  [APU-SELFLINK] terminated=%lu (guard %s)\n",
            g_apu_selflink_terminated,
            mcpx_apu_selflink_end() ? "on" : "OFF");
    /* mtf_head and mtf_deep are the two populations the ring fix splits the
     * re-inserts into, and they are printed beside head_nop because head_nop is
     * the guard mtf_head replaces: with move_to_front on, head_nop must read 0
     * and mtf_head must carry what it used to. The switch state goes in the
     * parentheses on this line because ab_score.py harvests exactly that from
     * [APU-TRAP], [APU-SELFLINK] and [APU-REON] to decide whether an arm
     * actually took -- a new line it does not know about would leave a
     * move_to_front A/B in the "assumes the environment took" class that the
     * SELFLINK_END A/B died of. */
    fprintf(stderr, "  [APU-REON] head_nop=%lu mtf_head=%lu mtf_deep=%lu"
            " mtf_inherit=%lu (reon_head_nop %s, move_to_front %s)\n",
            g_apu_voice_on_head_nop, g_apu_list_mtf_head, g_apu_list_mtf_deep,
            g_apu_list_mtf_inherit,
            mcpx_apu_reon_head_nop() ? "on" : "OFF",
            mcpx_apu_list_move_to_front() ? "on" : "OFF");
    fprintf(stderr, "  [APU-LINK] antecedent_sets=%lu on_top=%lu"
            " on_inherit=%lu self_ante=%lu self_link=%lu\n",
            g_apu_antecedent_set_count, g_apu_voice_on_top_count,
            g_apu_voice_on_inherit_count, g_apu_voice_on_self_ante_count,
            g_apu_voice_on_self_link_count);
    fprintf(stderr, "  [APU-CYCLE] relink=%lu (of %lu top inserts)"
            " walks_with_a_cycle=%lu broken=%lu last=v%u/list%u"
            " (cycle_break %s)\n",
            g_apu_voice_on_relink_count, g_apu_voice_on_top_count,
            g_apu_list_cycles_seen, g_apu_list_cycle_breaks,
            g_apu_list_cycle_last, g_apu_list_cycle_list,
            mcpx_apu_cycle_break() ? "on" : "OFF");
    /* The 256-iteration cap, which reported through a compiled-out DPRINTF from
     * the day it was written. On its own line rather than appended to
     * [APU-CYCLE] because it answers a different question -- walks_with_a_cycle
     * says a voice was reached twice, this says the walk ran out of iterations
     * before the list ended -- and because six months of notes grep the
     * [APU-CYCLE] line as it stands. Non-zero here with walks_with_a_cycle at
     * zero would mean a list genuinely longer than 256 entries, which is a
     * different bug from the ring. */
    fprintf(stderr, "  [APU-WALKCAP] hit=%lu\n", g_apu_list_walk_cap);
    mcpx_apu_idle_trap_report(0);
    fflush(stderr);
}

/* The idle-trap ring, with what each handle WAS when we raised it.
 *
 * Printed from here rather than from the harness so that the flag letters and
 * the code that sets them cannot drift apart. The harness's crash handler
 * calls the same function, which is the point: the periodic line and the line
 * in the crash dump are then the same line, and a run can be read forwards.
 *
 * The header text is unchanged from the harness's original, because scripts
 * and six months of notes grep for it.
 *
 * READING IT. Each entry is v<handle>[flags]<-<predecessor>. Flags:
 *   L  the voice was LOCKED -- the guest was inside VOICE_ON or VOICE_RELEASE
 *      for it, so it had published the voice but not finished it
 *   N  the guest has NEVER issued VOICE_ON for this handle in this run, so
 *      DirectSound cannot have an owner object for it
 *   P  CFG_FMT PERSIST is set, which means the guest's ISR returns early and
 *      this raise was harmless
 *   R  same handle as the previous raise
 * The predecessor is the voice whose link field pointed here; TVL means it was
 * the list head, reached straight off the top register.
 *
 * THE ONE THAT MATTERS is the LAST entry, because that is the handle the guest
 * was servicing when it died. L or N on that entry says the raise was ours to
 * withhold. Neither says it was not, and sends this back to the drawing board.
 *
 * raises is the positive control: locked=0 beside raises=0 says nothing at all
 * about whether the instrument works, so the two are printed together and must
 * be read together. */
void mcpx_apu_idle_trap_report(int crash)
{
    unsigned long r = g_idle_trap_ring;
    unsigned i, n, base;

    fprintf(stderr, "  [APU-IDLE-TRAP] raises=%lu locked_raises=%lu"
            " locked_enc=%lu never_on=%lu"
            " repeat=%lu lock_suppressed=%lu (lock_guard %s)\n",
            g_idle_trap_raises, g_idle_trap_locked_raises,
            g_idle_trap_locked_encounters,
            g_idle_trap_never_on_raises, g_idle_trap_repeat_raises,
            g_idle_trap_lock_suppressed,
            mcpx_apu_idle_trap_lock_guard() ? "on" : "OFF");

    /* PRINTED HERE, NOT IN THE PERIODIC REPORT, AND THAT IS THE POINT.
     *
     * This counter first went beside [APU-VOICE2] and read "0 owned, 0
     * UNOWNED" on a run that crashed with four raises. The counter was right;
     * it was printed by a function the CRASH PATH DOES NOT CALL, so the last
     * value on disk was from a periodic report taken before any raise had
     * happened. The four raises and the fault all landed in the gap.
     *
     * An instrument that does not print at the moment of interest measures
     * nothing, and it fails in the most expensive way available -- it reads
     * ZERO, which looks like an answer. It sits beside [APU-IDLE-TRAP] now
     * because that line is in the crash dump, verified by finding it there. */
    unsigned int dsound_this = jsrf_dsound_this();
    if (!dsound_this) {
        fprintf(stderr, "  [APU-IDLE-OWNER] NOT MEASURED -- the guest's"
                " DirectSound `this` was never resolved, so owner[h] was never"
                " read. It is the ServiceContext of the KINTERRUPT the title"
                " connects on vector 5 (the APU), so a zero here means the"
                " title had not connected it yet%s. This is NOT a zero"
                " result. (gen probe entered %lu time(s).)\n",
                g_jsrf_dsound_no_kernel
                    ? " -- except that it means NEITHER: the weak fallback"
                      " answered, so the kernel's interrupt table is not in"
                      " this link at all"
                    : "",
                g_jsrf_dsound_this_calls);
    } else {
        fprintf(stderr, "  [APU-IDLE-OWNER] this=0x%08X (from %s)  owner[h] at the raise:",
                dsound_this,
                g_jsrf_dsound_this_src == 1 ? "the gen probe"
                                            : "vector 5's ServiceContext");
        if (g_jsrf_dsound_this_disagree)
            fprintf(stderr, " -- WARNING: the probe (0x%08X) and the"
                    " KINTERRUPT (0x%08X) DISAGREE, so every owner[] count"
                    " below is read through one of two tables and has to be"
                    " retaken --", g_jsrf_dsound_this, g_jsrf_dsound_this_kint);
        fprintf(stderr, "\n  [APU-IDLE-OWNER] "
                " %lu non-NULL, %lu NULL. A NULL is a handle 001A200D"
                " dereferences unguarded, and it is fatal only for h==0 --"
                " which is every crash dump this project has. non-NULL>0 is"
                " the positive control for a NULL count of 0.\n",
                g_idle_owner_ok_raises, g_idle_owner_null_raises);
        fprintf(stderr, "  [APU-IDLE-OWNER]   of those NULLs, %lu were h==0"
                " (the only combination that reaches the faulting call);"
                " guard %s, %lu raise(s) withheld.\n",
                g_idle_owner_null_h0,
                owner_guard_on() ? "ON" : "OFF",
                g_idle_owner_suppressed);
        /* WHETHER THE LINE ABOVE IS ABOUT ONE OBJECT OR SEVERAL. The probe
         * captures `this` at ISR entry, so a raise reads owner[h] through
         * whatever the LAST entry left behind. One distinct value means that
         * staleness cannot matter and the counts stand; more than one means
         * they were taken against the wrong table and have to be retaken. */
        {
            unsigned long n = g_jsrf_dsound_this_distinct;
            unsigned i, lim = n < DSOUND_THIS_MAX ? (unsigned)n : DSOUND_THIS_MAX;
            fprintf(stderr, "  [APU-IDLE-OWNER]   `this` captured %lu time(s),"
                    " %lu distinct%s -- %s\n",
                    g_jsrf_dsound_this_calls, n,
                    g_jsrf_dsound_this_over ? " (table full, more exist)" : "",
                    n == 1 ? "ONE object, so the counts above are read through"
                             " the right table"
                           : "NOT ONE OBJECT: the raise-time counts above were"
                             " read through a table that is not necessarily"
                             " the ISR's, and must be retaken");
            if (lim) {
                fprintf(stderr, "  [APU-IDLE-OWNER]   values:");
                for (i = 0; i < lim; ++i)
                    fprintf(stderr, " 0x%08X", g_jsrf_dsound_this_seen[i]);
                fprintf(stderr, "\n");
            }
        }
    }
    /* THE SAME TABLE, READ AT THE HAND-OVER INSTEAD OF AT THE RAISE.
     *
     * reads is the positive control and has to be read first: it counts every
     * SE2FE_IDLE_VOICE method the guest actually collected through the read
     * trap, so reads=0 means the trap never fired on FEDECMETH and the two
     * numbers after it are silence, not zeros. null_h0 is the hypothesis --
     * the only combination that reaches sub_001A2E2E. Non-zero here beside a
     * zero on the raise-time line IS the time-of-check/time-of-use race,
     * measured. */
    fprintf(stderr, "  [APU-IDLE-OWNER]   at the HAND-OVER (the guest's"
            " FEDECMETH load): reads=%lu owned=%lu NULL=%lu h0=%lu"
            " withheld=%lu no-this=%lu (handoff_guard %s)%s\n",
            g_idle_handoff_reads, g_idle_handoff_owned, g_idle_handoff_null,
            g_idle_handoff_null_h0, g_idle_handoff_withheld,
            g_idle_handoff_nothis,
            idle_handoff_guard_on() ? "ON" : "OFF",
            g_idle_handoff_reads ? ""
              : "   <- reads=0: the FEDECMETH read trap never fired, so"
                " NULL=0 measures NOTHING");
    fprintf(stderr, "  [APU-FEDEC] guest methods dispatched while TRAPPED:"
            " %lu (of %lu) -- each one overwrites the FEDECMETH/FEDECPARAM"
            " pair the guest has not read yet\n",
            g_apu_method_while_trapped, g_apu_guest_method_count);
    fprintf(stderr, "  [APU-FEDEC] decode pairs held while trapped: %lu"
            " (fedec_hold %s) -- a held pair is an ISR that read the handle it"
            " was sent, not the next method's argument\n",
            g_apu_fedec_held,
            /* THIS LINE USED TO SAY "on (default)" WHEN THE GUARD IS OFF.
             * The accessor below reads `hold = e ? (atoi(e) != 0) : 0` -- the
             * default is 0 -- so an unset variable printed as enabled, and a
             * reader saw "held=0 (fedec_hold on (default))" and concluded the
             * guard was armed and had never needed to fire. The truth was that
             * it was off and had never once been exercised. That is exactly
             * the class CLAUDE.md means by "read a counter's trigger before
             * trusting its value", and it mattered here: this guard is the one
             * that would break the idle-trap storm, so its state is the first
             * thing anyone debugging that storm reads. */
            (getenv("RECOMP_APU_FEDEC_HOLD") && *getenv("RECOMP_APU_FEDEC_HOLD"))
                ? (atoi(getenv("RECOMP_APU_FEDEC_HOLD")) ? "on" : "OFF")
                : "on (default)");
    if (!g_idle_trap_raises)
        return;

    /* Same ring-read rule as everywhere else here: slot 0 is the oldest until
     * the ring wraps, after which the oldest is the one about to be written. */
    n = r < 16u ? (unsigned)r : 16u;
    base = r < 16u ? 0u : (unsigned)(r & 15u);
    /* THE HEADER BELOW STAYS UNIQUE TO THE CRASH DUMP.
     *
     * Six months of notes and more than one mining script do
     * `grep -m1 "LAST IDLE-VOICE TRAPS RAISED"` over a whole run log and read
     * the line after it as the state at the fault. Printing the same header
     * every ten seconds would hand all of them the FIRST report instead, and
     * they would not notice -- the line looks right. The periodic report gets
     * the same data under its own prefix. */
    if (crash)
        fprintf(stderr, "\nLAST IDLE-VOICE TRAPS RAISED (%lu total),"
                        " oldest first:\n  ", g_idle_trap_raises);
    else
        fprintf(stderr, "  [APU-IDLE-RING] last %u of %lu, oldest first: ",
                n, g_idle_trap_raises);
    for (i = 0; i < n; ++i) {
        unsigned k = (base + i) & 15u;
        uint8_t w = g_idle_trap_why[k];
        char flags[8];
        unsigned f = 0;
        if (w & IDLE_TRAP_WHY_LOCKED)   flags[f++] = 'L';
        if (w & IDLE_TRAP_WHY_NEVER_ON) flags[f++] = 'N';
        if (w & IDLE_TRAP_WHY_PERSIST)  flags[f++] = 'P';
        if (w & IDLE_TRAP_WHY_REPEAT)   flags[f++] = 'R';
        if (w & IDLE_TRAP_WHY_CYCLE)    flags[f++] = 'C';
        if (w & IDLE_TRAP_WHY_UNOWNED)  flags[f++] = 'O';
        flags[f] = 0;
        static const char *const lname[] = { "2D", "3D", "MP" };
        const char *ln = g_idle_trap_list[k] < 3 ? lname[g_idle_trap_list[k]]
                                                 : "??";
        if (g_idle_trap_from[k] == 0xFFFF)
            fprintf(stderr, " %s:v%u[%s]<-TVL%s", ln,
                    (unsigned)g_idle_trap_last[k],
                    flags, g_idle_trap_fmt[k] ? "" : " fmt=0");
        else
            fprintf(stderr, " %s:v%u[%s]<-v%u%s", ln,
                    (unsigned)g_idle_trap_last[k],
                    flags, (unsigned)g_idle_trap_from[k],
                    g_idle_trap_fmt[k] ? "" : " fmt=0");
    }
    if (!crash) {
        fprintf(stderr, "\n");
        return;
    }
    fprintf(stderr, "\n  The guest ISR dereferences its own object for"
                    " the handle it is handed.\n"
                    "  L = locked (guest mid-VOICE_ON/RELEASE),"
                    " N = never VOICE_ON in this run, P = PERSIST (ISR returns"
                    " early), R = repeat of the previous raise,"
                    " C = THIS VOICE WAS SEEN TWICE IN ONE LIST WALK"
                    " (it is in a ring; see [APU-CYCLE]),"
                    " O = THE GUEST DOES NOT OWN THIS VOICE NOW (VOICE_ON"
                    " seen, then VOICE_RELEASE, no VOICE_ON since) -- 001A200D"
                    " reads owner[h] with NO NULL CHECK, so an O on the LAST"
                    " entry of a crashing run is the fault explained.\n"
                    "  2D/3D/MP is the voice list. fmt=0 is an unconfigured"
                    " voice. The LAST entry is the"
                    " handle the guest was servicing.\n");
}

static void voice_off(MCPXAPUState *d, uint16_t v)
{
    voice_lifecycle_note(d, v, "retire");
    g_apu_voice_off_count++;
    voice_ev_note(1, v);
    if (v < MCPX_HW_MAX_VOICES) g_pool_off_window[v / 64] |= 1ULL << (v % 64);

    /* RETIRING A VOICE THAT WAS ALREADY RETIRED posts a SECOND
     * MCPX_HW_NOTIFIER_SSLA_DONE for it, into guest memory, and re-raises
     * FEVINTSTS|FENINTSTS. If DirectSound has already torn down the object
     * behind that buffer, the second notification arrives for something that
     * no longer exists -- a route to the same class of fault as the idle trap
     * and entirely independent of it.
     *
     * Reported rather than suppressed, because suppressing it is a guess about
     * hardware and this model has already been burned by shipping one of those
     * (see the lock guard). The arithmetic says it happens: off=415 against
     * on=409 in gpuvsh565, so at least six voices were retired twice.
     * Positive control: g_apu_voice_off_count, nonzero in every run. */
    if (!voice_get_mask(d, v, NV_PAVS_VOICE_PAR_STATE,
                        NV_PAVS_VOICE_PAR_STATE_ACTIVE_VOICE))
        g_apu_voice_off_already_count++;

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
    /* DO NOT CLOBBER AN UNREAD DECODE PAIR. RECOMP_APU_FEDEC_HOLD, default
     * OFF -- this comment said "default on" until 17 Sep 2026 and was wrong.
     * The accessor below is `hold = e ? (atoi(e) != 0) : 0`. The report line
     * had already been corrected to print "OFF (default)"; this one had not,
     * so the two disagreed and this is the one a reader reaches first.
     *
     * IT IS NOT A THEORETICAL WINDOW. On 17 Sep 2026 the title crashed inside
     * the guest's DirectSound ISR shortly after New Game, with 62 of 2,555
     * guest methods dispatched while the front end was TRAPPED and the guard
     * off. The crash dump's own line says it: "The guest ISR dereferences its
     * own object for the handle it is handed".
     *
     * The window this closes is measured, not argued. fe_method writes
     * FEDECMETH and FEDECPARAM for EVERY method, guest methods reach it on a
     * guest thread with no lock, and the frame thread writes the same two
     * registers when it raises SE2FE_IDLE_VOICE. The guest's ISR reads them as
     * two separate MMIO loads back to back -- 001A25AA takes FEDECMETH into
     * ecx, then FEDECPARAM into esi -- and only THEN tests the method against
     * 0x8000. A guest method landing between those two loads leaves the ISR
     * holding our 0x8000 with somebody else's argument, and the argument of a
     * method like SET_ANTECEDENT_VOICE is a voice handle, so it sails through
     * the ISR's `h >= 0x100` guard and is dereferenced.
     *
     * MEASURED 16 Sep 2026 at gameplay: 39 guest methods arrived while the
     * front end was TRAPPED, out of 8292. g_apu_guest_method_count is the
     * positive control for that -- a zero beside a zero would mean only that
     * no methods arrived.
     *
     * WHY HOLDING IS THE HARDWARE BEHAVIOUR, not a patch: a trapped front end
     * has stopped decoding, so the pair holds still until the guest resumes
     * it. Ours decodes straight through, which is the defect. The method's own
     * side effects still run; only the register pair is held, because that
     * pair is the thing the guest is in the middle of reading.
     *
     * WHAT THIS DOES NOT CLAIM. It closes a corruption window whose signature
     * matches the crash -- DirectSound's RemoveIdleVoice called with a NULL
     * this, 40 of 66 recorded guest faults in this tree. It is NOT proven to
     * eliminate that crash: the crash fires in about one run in five, so
     * showing it gone needs a run count nobody has taken yet. The measured
     * claim is the window, not the cure.
     *
     * The sibling hypothesis -- VOICE_ON publishing a handle before marking it
     * active -- is DEAD, and this is what replaced it. The idle-trap ring it
     * was given reports locked=0 and never_on=0 over a gameplay run, which by
     * its own decision rule rules it out. */
    {
        /* ALSO OFF BY DEFAULT, and for a weaker reason than the lock guard:
         * it has never been run ALONE. Both guards were defaulted on in the
         * same commit and the next four runs all faulted at t=24.03, so the
         * regression cannot be attributed to either one. This is the cheaper
         * half to exonerate -- one run with FEDEC_HOLD=1 and the lock guard
         * off -- and until somebody takes it, shipping it on would be
         * shipping an unattributed change.
         *
         * THE ATTRIBUTION ARRIVED 17 Sep 2026. With the guard off the title
         * crashed inside the guest's DirectSound ISR shortly after New Game
         * (guest stack ending 001A25D9, 62 of 2,555 methods dispatched while
         * trapped). With it on, the next player session ran to completion with
         * held=1702 of 1,702 and no crash. That is the attribution, so it
         * ships on. */
        /* DEFAULT ON since 17 Sep 2026. `(e && *e)` rather than `e`, because
         * the switch audit found nine default-on sites that read an EMPTY
         * value as off; RECOMP_APU_FEDEC_HOLD= should mean "unset", not
         * "disabled". RECOMP_APU_FEDEC_HOLD=0 turns it off for an A/B. */
        static int hold = -1;
        if (hold < 0) { const char *e = getenv("RECOMP_APU_FEDEC_HOLD");
                        hold = (e && *e) ? (atoi(e) != 0) : 1; }
        if (hold && (qatomic_read(&d->regs[NV_PAPU_FECTL])
                     & NV_PAPU_FECTL_FEMETHMODE)
                    == NV_PAPU_FECTL_FEMETHMODE_TRAPPED) {
            ++g_apu_fedec_held;
        } else {
            d->regs[NV_PAPU_FEDECMETH] = method;
            d->regs[NV_PAPU_FEDECPARAM] = argument;
        }
    }
    unsigned int selected_handle, list;

    switch (method) {
    case NV1BA0_PIO_VOICE_LOCK:
        voice_lock(d, (uint16_t)d->regs[NV_PAPU_FECV], argument & 1);
        break;

    case NV1BA0_PIO_SET_ANTECEDENT_VOICE:
        g_apu_antecedent_set_count++;
        d->regs[NV_PAPU_FEAV] = argument;
        break;

    case NV1BA0_PIO_VOICE_ON: {
        g_apu_voice_on_count++;
        selected_handle = argument & NV1BA0_PIO_VOICE_ON_HANDLE;
        /* Recorded before anything can fail below: the question this answers
         * is "has DirectSound ever owned this handle", and the answer becomes
         * yes the moment the guest asks for the voice, not when we finish
         * setting it up. */
        if (selected_handle < MCPX_HW_MAX_VOICES) {
            g_apu_voice_ever_on[selected_handle / 64] |=
                1ULL << (selected_handle % 64);
            /* "owns it now" -- see g_apu_voice_owned_now. */
            g_apu_voice_owned_now[selected_handle / 64] |=
                1ULL << (selected_handle % 64);
        }
        voice_lifecycle_note(d, (uint16_t)selected_handle, "on");
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

        /* Read-only provenance capture. feav_before is the register as the
         * insert found it, so the log records what decided the branch rather
         * than what the branch left behind. */
        uint32_t feav_before = d->regs[NV_PAPU_FEAV];
        unsigned int ante_before = GET_MASK(feav_before, NV_PAPU_FEAV_VALUE);
        uint32_t link_before = voice_get_mask(
            d, (uint16_t)selected_handle, NV_PAVS_VOICE_TAR_PITCH_LINK,
            NV_PAVS_VOICE_TAR_PITCH_LINK_NEXT_VOICE_HANDLE);
        link_shadow_check((uint16_t)selected_handle, link_before, "voice-on");

        list = GET_MASK(d->regs[NV_PAPU_FEAV], NV_PAPU_FEAV_LST);
        if (list != NV1BA0_PIO_SET_ANTECEDENT_VOICE_LIST_INHERIT) {
            unsigned int top_reg = voice_list_regs[list - 1].top;
            g_apu_voice_on_top_count++;
            /* PREPENDING THE VOICE THAT IS ALREADY THE HEAD IS A NO-OP ON THE
             * LIST, AND DOING IT AS A WRITE DESTROYS THE LIST.
             *
             * link(v) = regs[top] with regs[top] == v stores v's own handle
             * over its real successor. The list becomes a one-entry cycle whose
             * single entry is a voice that is about to go inactive again, and
             * everything behind it is unreachable -- not merely unrendered,
             * GONE, because the successor handle lived only in the field that
             * was just overwritten.
             *
             * The walk then spins on that cycle for its full 256-iteration cap,
             * every subframe, for the rest of the run. Measured in the player's
             * own session 20260915-161819: voice 3 taking 51,665 of 52,884 idle
             * traps, voice_process calls falling 14-fold, VOICE_ON from the
             * guest stopping entirely, and the title going SILENT with a
             * perfectly healthy output device -- out_hz 48003, starved=0,
             * empty=0. We were feeding the card silence.
             *
             * WHY THIS IS THE RIGHT PLACE. regs[top] is not stale: the guest
             * re-ONs a voice that genuinely IS its list's head, which is an
             * ordinary thing to do with a DirectSound buffer whose voice has
             * stopped but has not yet been taken out of the hardware list.
             * Sixteen cycle-forming inserts across four instrumented runs, and
             * in every one of them link(v) held a real successor at the moment
             * we overwrote it. See
             * docs/jsrf/progress/CLAUDE_PROGRESS_2026-09-15_THE_HEAD_WAS_NEVER_STALE.md.
             *
             * AND IT DOES NOT CONDITION AWAY THE TOP WRITE. The objection to
             * touching this insert was that CMcpxVoiceClient's debug validation
             * asserts dwTVL equals its head voice, so suppressing the write
             * would break every head insertion. That assert is UNAFFECTED here:
             * in this branch regs[top] already IS selected_handle, so leaving
             * both fields alone leaves dwTVL naming the head exactly as the
             * driver requires. Every other insertion takes the original path
             * untouched.
             *
             * RECOMP_APU_REON_HEAD_NOP=0 restores the overwrite for A/B. */
            /* BEFORE the insert, because after it the answer is always yes.
             * head_nop's guard only covers regs[top]==selected; this says how
             * often selected was already reachable from somewhere DEEPER,
             * which is the case that forms a ring of length >= 2 and the case
             * nothing here has ever counted. */
            if (voice_list_contains(d, top_reg, (uint16_t)selected_handle))
                g_apu_voice_on_relink_count++;

            /* TAKE v OUT BEFORE PUTTING IT BACK IN.
             *
             * The long-form argument, the measured cost of not doing this and
             * the race it does not close are all above
             * mcpx_apu_list_move_to_front; what follows is only what the two
             * branches do.
             *
             *   pred == 0xFFFF   v is already this list's head. Unlinking would
             *                    write regs[top] = link(v), and the prepend
             *                    below would then write link(v) = regs[top] =
             *                    link(v) and regs[top] = v -- the same two
             *                    values back again. So do neither: the final
             *                    state is identical, and skipping both stores
             *                    also means there is no instant in which the
             *                    list does not contain v for the frame thread
             *                    to walk into. This is RECOMP_APU_REON_HEAD_NOP
             *                    by construction rather than by arithmetic,
             *                    which is the sense in which this switch
             *                    subsumes it exactly.
             *
             *   otherwise        v is deeper in. voice_list_unlink_at writes
             *                    link(pred) = link(v), so v's successor stays
             *                    attached to v's old predecessor, and the
             *                    prepend below then moves v to the head.
             *                    Nothing becomes unreachable -- which is the
             *                    entire difference from what this branch did.
             *
             * The chain below is left intact and still reads reon_head_nop, so
             * that with move_to_front OFF this insert is byte-for-byte the code
             * that was here before: the switch adds one bounded walk when it is
             * ON and nothing at all when it is OFF.
             *
             * selected_handle is range-checked because VOICE_ON masks the
             * handle with 0x0000FFFF (apu_regs.h:137) while the hardware has
             * 256 voices. An out-of-range handle cannot be in a list, and
             * voice_get_mask on one would address guest RAM past the end of the
             * voice register file. */
            int mtf_is_head = 0;
            if (mcpx_apu_list_move_to_front()
                && selected_handle < MCPX_HW_MAX_VOICES) {
                uint16_t mtf_pred = 0xFFFF;
                if (voice_list_find_pred(d, top_reg,
                                         (uint16_t)selected_handle,
                                         &mtf_pred)) {
                    if (mtf_pred == 0xFFFF) {
                        g_apu_list_mtf_head++;
                        mtf_is_head = 1;
                    } else {
                        g_apu_list_mtf_deep++;
                        voice_list_unlink_at(d, top_reg, mtf_pred,
                                             (uint16_t)selected_handle);
                    }
                }
            }

            if (mtf_is_head) {
                /* Nothing to do. v is the head already, so the insert the
                 * guest asked for is the state the list is in. Counted as
                 * mtf_head on the [APU-REON] line. */
            } else if (d->regs[top_reg] == selected_handle
                       && mcpx_apu_reon_head_nop()) {
                g_apu_voice_on_head_nop++;
            } else {
                voice_set_mask(d, (uint16_t)selected_handle,
                               NV_PAVS_VOICE_TAR_PITCH_LINK,
                               NV_PAVS_VOICE_TAR_PITCH_LINK_NEXT_VOICE_HANDLE,
                               d->regs[top_reg]);
                d->regs[top_reg] = selected_handle;
            }
        } else {
            unsigned int antecedent_voice =
                GET_MASK(d->regs[NV_PAPU_FEAV], NV_PAPU_FEAV_VALUE);
            assert(antecedent_voice != 0xFFFF);

            g_apu_voice_on_inherit_count++;
            if (antecedent_voice == selected_handle)
                g_apu_voice_on_self_ante_count++;

            /* THE SAME DEFECT, ONE BRANCH OVER.
             *
             * The splice below writes link(selected) = link(antecedent) and
             * then link(antecedent) = selected. If selected is already in a
             * list, the first of those stores lands on the only pointer to
             * selected's successor and the second splices selected in behind an
             * antecedent that may still reach it. That is the TOP branch's ring
             * reached by different arithmetic, and it is fixed the same way:
             * unlink first, then splice.
             *
             * THIS BUYS JSRF NOTHING AND COSTS IT NOTHING. on_inherit is 0 --
             * and self_ante with it -- in all 5,372 [APU-LINK] report lines
             * across the 268 run logs this tree has kept, because FEAV reads
             * 0x0001FFFF or 0x0002FFFF in all 535 [VOICE-LINK] lines of
             * play/20260915-122828-human-gameplay/stderr.log: ante is always
             * the FFFF that selects a TOP insert. It is here so the two
             * insert paths cannot disagree about what re-inserting a voice
             * means, and so that the next title through does not meet the bug
             * we just took out of the other branch.
             *
             * INHERIT names no list, so there is no top register to start from
             * and no way to know which of the three holds selected: all three
             * are searched with the same bounded walk and the first that
             * reaches it wins. A handle in two lists at once is corruption no
             * insert can repair, and this does not pretend to.
             *
             * SELF-ANTECEDENT IS LEFT EXACTLY AS IT WAS. FEAV naming the voice
             * being turned on collapses the splice to link(v) = v, and
             * "unlink v, then insert v after v" is not a repair of that, it is
             * a different wrong answer -- it would drop v from its list and
             * leave the sentinel set. self_ante=0 in all 5,372 of those report
             * lines; the counter one line above is what would say otherwise. */
            if (mcpx_apu_list_move_to_front()
                && selected_handle < MCPX_HW_MAX_VOICES
                && antecedent_voice != selected_handle) {
                int mtf_l;
                for (mtf_l = 0; mtf_l < 3; mtf_l++) {
                    unsigned mtf_top = (unsigned)voice_list_regs[mtf_l].top;
                    uint16_t mtf_pred = 0xFFFF;
                    if (!voice_list_find_pred(d, mtf_top,
                                              (uint16_t)selected_handle,
                                              &mtf_pred))
                        continue;
                    g_apu_list_mtf_inherit++;
                    voice_list_unlink_at(d, mtf_top, mtf_pred,
                                         (uint16_t)selected_handle);
                    break;
                }
            }

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

        /* The outcome, read back rather than predicted. Nothing below changes
         * the link, so this is the state the walk will meet. */
        uint32_t link_after = voice_get_mask(
            d, (uint16_t)selected_handle, NV_PAVS_VOICE_TAR_PITCH_LINK,
            NV_PAVS_VOICE_TAR_PITCH_LINK_NEXT_VOICE_HANDLE);
        if (link_after == selected_handle) g_apu_voice_on_self_link_count++;
        link_shadow_set((uint16_t)selected_handle, link_after);
        voice_link_note(d, (uint16_t)selected_handle, feav_before, list,
                        ante_before, link_before, link_after);

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
        if (selected_handle < MCPX_HW_MAX_VOICES) {
            uint64_t pbit = 1ULL << (selected_handle % 64);
            if (voice_get_mask(d, (uint16_t)selected_handle,
                               NV_PAVS_VOICE_PAR_STATE,
                               NV_PAVS_VOICE_PAR_STATE_ACTIVE_VOICE))
                g_pool_on_active++;
            if (g_pool_off_window[selected_handle / 64] & pbit) g_pool_reuse++;
            g_pool_on_window[selected_handle / 64] |= pbit;
            if (selected_handle < MCPX_HW_MAX_3D_VOICES) g_pool_on_3d++;
            else g_pool_on_2d++;
        }
        voice_set_mask(d, (uint16_t)selected_handle, NV_PAVS_VOICE_PAR_STATE,
                       NV_PAVS_VOICE_PAR_STATE_ACTIVE_VOICE, 1);

        /* RE-ARM THE IDLE EDGE. The guest has just restarted this voice, so
         * whatever we last told it about this handle is spent: the next time
         * the voice goes idle it is a new transition and the guest is entitled
         * to hear about it. Unconditional, so that a run with
         * RECOMP_APU_IDLE_TRAP_EDGE off still reports how often the latch
         * would have been cycling -- a latch that never re-arms is a latch
         * that would swallow every retirement after the first, and that is the
         * failure this switch has to be able to show is not happening. */
        if (selected_handle < MCPX_HW_MAX_VOICES) {
            uint64_t ebit = 1ULL << (selected_handle % 64);
            if (g_idle_trap_reported[selected_handle / 64] & ebit) {
                g_idle_trap_reported[selected_handle / 64] &= ~ebit;
                g_idle_trap_edge_rearm++;
            }
        }

        if (!locked) voice_lock(d, (uint16_t)selected_handle, false);
        break;
    }

    case NV1BA0_PIO_VOICE_RELEASE: {
        g_apu_voice_release_count++;
        selected_handle = argument & NV1BA0_PIO_VOICE_ON_HANDLE;
        /* See g_apu_voice_owned_now. Clearing here is the INFERENCE under
         * test: this handler sets envelope release rates, so it may be a
         * fade-out rather than an object free. If the guest still owns a
         * releasing voice, `unowned` reads 0 on a crashing run and the
         * hypothesis dies. */
        if (selected_handle < MCPX_HW_MAX_VOICES)
            g_apu_voice_owned_now[selected_handle / 64] &=
                ~(1ULL << (selected_handle % 64));
        voice_lifecycle_note(d, (uint16_t)selected_handle, "release-command");
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
        /* CLEARED HERE TOO, AND THIS IS THE ONE THAT MATTERS.
         *
         * The first version of this cleared only on VOICE_RELEASE and
         * reported "15987 owned, 0 UNOWNED" -- which proved nothing, because
         * the same report read `on=288 off=258 release=0`. VOICE_RELEASE never
         * fired in the whole run, so the clear side was never exercised and
         * the zero was the instrument's silence, not an answer. This title
         * retires voices with VOICE_OFF. */
        if ((argument & NV1BA0_PIO_VOICE_OFF_HANDLE) < MCPX_HW_MAX_VOICES)
            g_apu_voice_owned_now[(argument & NV1BA0_PIO_VOICE_OFF_HANDLE) / 64]
                &= ~(1ULL << ((argument & NV1BA0_PIO_VOICE_OFF_HANDLE) % 64));
        g_apu_voice_off_command_count++;
        voice_lifecycle_note(d, (uint16_t)(argument & NV1BA0_PIO_VOICE_OFF_HANDLE),
                             "off-command");
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
        /* FECV is stored unmasked (see SET_CURRENT_VOICE) and every other
         * reader casts it to uint16_t. This one did not, and indexed
         * d->vp.filters[] with the signed result guarded only from above -- a
         * negative value passed the test and wrote out of bounds. Match the
         * other readers. This title never writes a handle >= 0x100, so the
         * defect is a code property rather than a live bug; the disagreement
         * between the two idioms is the thing being fixed. */
        unsigned current_voice = (uint16_t)d->regs[NV_PAPU_FECV];
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
        if (d->regs[NV_PAPU_FECV] < MCPX_HW_MAX_VOICES) {
            g_voice_guest_ba[d->regs[NV_PAPU_FECV]] =
                argument & NV1BA0_PIO_SET_VOICE_CFG_BUF_BASE_OFFSET;
            ++g_voice_guest_ba_sets[d->regs[NV_PAPU_FECV]];
        }
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
        if (d->regs[NV_PAPU_FECV] < MCPX_HW_MAX_VOICES) {
            g_voice_guest_ebo[d->regs[NV_PAPU_FECV]] =
                argument & NV1BA0_PIO_SET_VOICE_CFG_BUF_EBO_OFFSET;
            ++g_voice_guest_ebo_sets[d->regs[NV_PAPU_FECV]];
        }
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
        g_bin_submix[0] = (unsigned char)((argument >> 0) & 0x1f);
        g_bin_submix[1] = (unsigned char)((argument >> 8) & 0x1f);
        g_bin_submix[2] = (unsigned char)((argument >> 16) & 0x1f);
        g_bin_submix[3] = (unsigned char)((argument >> 24) & 0x1f);
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
            /* Unknown method.
             *
             * This was a DPRINTF, which compiles to nothing, so every method
             * the model does not decode was dropped leaving no trace and no
             * count. That is the wrong shape for a front end whose job is to
             * receive guest commands: "the guest never asked" and "we ignored
             * the ask" read identically from every log in the repository.
             *
             * Records distinct method numbers so the report can name them. It
             * changes no behaviour -- the method is still ignored. */
            g_apu_unknown_method_count++;
            {
                unsigned k;
                for (k = 0; k < g_apu_unknown_method_n; k++)
                    if (g_apu_unknown_method[k] == method) break;
                if (k == g_apu_unknown_method_n &&
                    g_apu_unknown_method_n < APU_UNKNOWN_METHOD_MAX)
                    g_apu_unknown_method[g_apu_unknown_method_n++] = method;
            }
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

    g_apu_guest_method_count++;
    /* THE OTHER WAY THE GUEST CAN BE HANDED THE WRONG HANDLE, measured rather
     * than argued about.
     *
     * fe_method writes FEDECMETH and FEDECPARAM for EVERY method, and this
     * function is how guest methods reach it -- on a guest thread, with no
     * lock. The frame thread writes the same two registers when it raises
     * SE2FE_IDLE_VOICE. The guest's ISR reads them as two separate MMIO loads,
     * back to back (001A25AA: FEDECMETH into ecx, then FEDECPARAM into esi),
     * and only then tests the method against 0x8000. A guest method landing
     * between those two loads leaves the ISR with our 0x8000 and somebody
     * else's argument -- and the argument of a method like
     * SET_ANTECEDENT_VOICE is a voice handle, so it passes the ISR's
     * `h >= 0x100` guard and is dereferenced.
     *
     * On hardware the window does not exist: a trapped front end has stopped
     * decoding, so the pair holds still until the guest resumes it. Ours
     * decodes straight through.
     *
     * This counts only arrivals while the front end is TRAPPED, which is
     * exactly the interval in which an unread pair is outstanding. Zero over a
     * whole run kills the hypothesis outright, and g_apu_guest_method_count is
     * the positive control -- a zero here beside a zero there means only that
     * no methods arrived at all. Counter only; nothing is changed. */
    if ((qatomic_read(&d->regs[NV_PAPU_FECTL]) & NV_PAPU_FECTL_FEMETHMODE)
            == NV_PAPU_FECTL_FEMETHMODE_TRAPPED)
        g_apu_method_while_trapped++;

    /* Dispatch known methods through fe_method */
    fe_method(d, (uint32_t)addr, (uint32_t)val);
}

/* ============================================================
 * SGE data pointer resolution
 * ============================================================ */

/* Scatter-gather translations whose page-table entry did not look like one.
 *
 * G7. `max_sge` is the bound, and EVERY call site passes 0xFFFFFFFF, so the
 * assert below has never been able to fire. A read past the end of the voice
 * processor's page table therefore returns a translation built from whatever
 * dword happens to sit at that offset -- no error, no counter, and an address
 * plausible enough to read from.
 *
 * The honest bound is the table's length, and nothing in the register file
 * gives it to us: NV_PAPU_VPSGEADDR is a base with no companion limit. So
 * this checks the ENTRY instead of the index. A real PRD entry is a 4 KB
 * page address inside guest RAM; zero, unaligned, or out of range means we
 * read past the table and are about to translate through rubbish.
 *
 * COUNTED, NOT REFUSED, by default. Substituting a different address is a
 * behaviour change, and this tree ships those off -- see the adpcm_guard
 * comment for the same argument. RECOMP_APU_SGE_GUARD=1 makes it return 0
 * instead, which the caller will read as a zero page rather than as audio.
 *
 * WHAT THIS IS NOT EVIDENCE FOR, yet. The ADPCM over-read is the reason this
 * exists, but the failure-ring note a few hundred lines down already states
 * the discriminator, and the 19 Sep data leans the other way: the failing
 * pages are many and various (54, 56, 61, 63, 66, 81, 90, 104) and their PRDs
 * are plausible and page-aligned, which is that note's "the buffer is being
 * reused underneath us" reading rather than its "the translation is wrong
 * past that page" one. If sge_oob stays at 0 across a run that still refuses
 * 4% of its ADPCM blocks, G7 is not the cause and should be written off. */
unsigned long g_apu_sge_oob, g_apu_sge_calls;

static int apu_sge_guard(void)
{
    static int on = -1;
    if (on < 0) on = recomp_switch_on("RECOMP_APU_SGE_GUARD");
    return on;
}

static hwaddr get_data_ptr(hwaddr sge_base, unsigned int max_sge, uint32_t addr)
{
    unsigned int entry = addr / TARGET_PAGE_SIZE;
    assert(entry <= max_sge);
    uint32_t prd_address =
        ldl_le_phys(address_space_memory, sge_base + entry * 4 * 2);
    ++g_apu_sge_calls;
    if (prd_address == 0
            || (prd_address & (TARGET_PAGE_SIZE - 1)) != 0
            || (prd_address & ~g_apu_ram_mask) != 0) {
        ++g_apu_sge_oob;
        if (apu_sge_guard())
            return 0;
    }
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
int mcpx_apu_se_while_trapped(void);

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

/* THE ADPCM DECODE HAS NEVER BEEN CHECKED, AND HAS NEVER BEEN COUNTED.
 *
 * adpcm_decode_block (apu_state.h) returns the number of frames it produced,
 * and 0 for a block it refuses. It refuses in two places:
 *
 *   * inbufsize < channels * 4  -- before writing anything at all;
 *   * index[ch] out of 0..88, or the fourth header byte non-zero -- AFTER it
 *     has already stored that channel's first sample and advanced outbuf.
 *
 * The call site ignored the return value entirely and read 64 frames out of
 * `int16_t adpcm_decoded[65 * 2]`, an UNINITIALISED automatic array. So a
 * refused block emits up to 129 int16 of whatever was on the stack, converted
 * by int16_to_float and mixed at full scale. And because adpcm_block_index was
 * set unconditionally straight afterwards, the refusal was cached: every one
 * of the block's 64 samples came out of the same garbage, and the block was
 * never retried.
 *
 * WHAT THIS IS AND IS NOT. It is NOT the reason sound effects are silent --
 * the established finding is that SE voices are never STARTED, and garbage
 * mixed at full scale is loud, not quiet. It is a separate defect whose
 * symptom is noise, and JSRF does use ADPCM, so it can fire. The counters are
 * unconditional and decide in one run whether it is firing at all; the repair
 * is behind a switch because substituting silence for garbage is a behaviour
 * change and this tree's rule is that those ship off.
 *
 * _fail and _ok share a denominator (blocks decoded) and are printed together,
 * so _ok is the positive control: fail=0 beside ok=0 means no ADPCM voice ever
 * played and says nothing about the decoder.
 *
 * _short is a block the decoder accepted but for which it produced fewer than
 * ADPCM_SAMPLES_PER_BLOCK frames -- the tail of the read is then uninitialised
 * for exactly the same reason, with no error to notice.
 *
 * _oversize is the one that is repaired unconditionally, because it is memory
 * corruption rather than bad audio. block_size is 36 * samples_per_block and
 * NV_PAVS_VOICE_CFG_FMT_SAMPLES_PER_BLOCK is five bits, so block_size reaches
 * 1152 -- copied by memcpy into `uint32_t adpcm_block[18]`, 72 bytes, on the
 * stack. The decoder then overruns `adpcm_decoded` to match: 72 bytes of mono
 * input yields 137 frames into a 130-entry array. Clamping the fetch to one
 * 36-byte block per channel bounds both, and loses nothing readable -- the
 * reader below indexes block_position < ADPCM_SAMPLES_PER_BLOCK frames, and
 * 36 bytes per channel is exactly 65 frames per channel. If _oversize reads 0
 * over a run, this clamp changed nothing in it. */
unsigned long g_apu_adpcm_ok;
unsigned long g_apu_adpcm_fail;
unsigned long g_adpcm_fail_ring, g_adpcm_fail_first, g_adpcm_fail_last, g_adpcm_fail_mid;
uint16_t g_adpcm_fail_v[8]; uint8_t g_adpcm_fail_stream[8], g_adpcm_fail_ch[8];
uint32_t g_adpcm_fail_block[8], g_adpcm_fail_nblocks[8], g_adpcm_fail_hdr[8];
/* THE PAGE THE FAILING BLOCK LANDED IN, AND WHAT THE TABLE SAID ABOUT IT.
 *
 * get_data_ptr's bounds parameter is defeated at every call site (max_sge is
 * 0xFFFFFFFF), so a read past the end of the voice processor's page table
 * returns a translation built from whatever dword sits at that offset, with
 * no error and no counter. The ADPCM failures fit that: block 0 of a buffer
 * NEVER fails, and every observed failing block is above 113 -- which is
 * 4096/36, the number of 36-byte ADPCM blocks in one 4 KB page.
 *
 * This records, per failure, the linear address, its page index, and the
 * page address the table returned, plus the LOWEST failing block index per
 * voice. Read them together:
 *   lowest failing block ~= a page multiple, prd implausible beyond it
 *        -> the translation is wrong past that page. Carry a real bound.
 *   failures scattered across pages with plausible prd
 *        -> the buffer is being reused underneath us; this dies too.
 * Read-only: it recomputes the address rather than touching the fetch. */
uint32_t g_adpcm_fail_page[8], g_adpcm_fail_prd[8], g_adpcm_fail_lin[8];
/* THE DISCRIMINATOR THE GAP NEEDS. Every voice starts failing exactly 11
 * blocks before the end of its buffer (see the 19 Sep progress note), which
 * is a fixed over-read -- but two different faults produce it and they have
 * different fixes:
 *
 *   the BASE is 11 blocks too high            -> ba / block_size == 11
 *   the BUFFER is 11 blocks shorter than ebo  -> ba / block_size == 0-ish
 *                                                and ebo is the wrong field
 *
 * Neither is decidable from what this ring printed, because it recorded the
 * translated address and not the two numbers the address was built from.
 * Recording them makes the next gameplay run answer it off one line. */
uint32_t g_adpcm_fail_bs[8], g_adpcm_fail_ba[8], g_adpcm_fail_ebo[8];
unsigned long g_adpcm_fail_page0, g_adpcm_fail_pagehi;
uint32_t g_adpcm_first_fail_blk[MCPX_HW_MAX_VOICES];
/* The buffer's block count beside the first block that failed in it. Without
 * this the per-voice line cannot be read at all: a first failure at block 208
 * means nothing until you know the buffer holds 219. See the report. */
uint32_t g_adpcm_fail_nblocks_v[MCPX_HW_MAX_VOICES];
unsigned long g_apu_adpcm_short;
unsigned long g_apu_adpcm_oversize;
unsigned long g_apu_adpcm_silenced;

/* WHERE THE DATA ACTUALLY IS. RECOMP_APU_ADPCM_EXTENT=1, default OFF.
 *
 * The two theories left standing produce the SAME signature -- failures
 * beginning exactly 11 blocks before the end of every buffer, whatever its
 * length and whatever its block size -- and the discriminator this file
 * already carries cannot separate them, because it tried to read the answer
 * out of `ba / block_size` and `ba` is an absolute offset in the voice's
 * scatter-gather space, not an error term. Both readings it predicted are
 * refuted (5389 and 1051, against 11 and 0).
 *
 *   the BASE is 11 blocks too high    -> the audio starts at ba - 11*bs, so
 *                                        blocks -1 .. -11 hold real ADPCM
 *   the BUFFER is 11 blocks shorter   -> nothing decodable below block 0, and
 *                                        the tail is simply never written
 *
 * They differ in ONE observable and it costs a read: what is immediately
 * BELOW the base. So walk the buffer with the model's own translation and ask
 * each block whether adpcm_decode_block would accept it -- reserved byte zero
 * and step index 0..88, which is the decoder's exact refusal test and nothing
 * more. Random bytes pass that with probability about 1/300 per channel, so a
 * run of eleven consecutive passes below the base is not chance.
 *
 * WHAT IT IS NOT. It is not a fix and it does not touch the fetch: the scan
 * recomputes addresses beside the real one, reads through ldl_le_phys, and
 * writes only its own record. It runs ONCE per voice, on that voice's first
 * refusal, so its cost is a few hundred loads a session.
 *
 * It does add to g_apu_sge_calls, because it translates through get_data_ptr
 * on purpose -- a scan that used a different translation would be measuring a
 * different bug. Leave it off when reading [APU-SGE].
 *
 * The 0x08 run is recorded because every failure on record carries the header
 * 0x08080808, and "the tail is fill" is only established if the fill is
 * actually there and actually stops somewhere. */
#define ADPCM_EXTENT_SLOTS   8
#define ADPCM_EXTENT_BELOW   40   /* blocks probed below the base */
#define ADPCM_EXTENT_ABOVE   40   /* blocks probed past the nominal end */

typedef struct {
    uint16_t v;
    uint8_t  used, stream, channels;
    uint32_t bs, ba, ebo, nblocks, first_fail;
    uint32_t seg;                 /* segment_offset, stream voices only */
    uint32_t good_below;          /* consecutive decodable blocks below base */
    uint32_t good_in_buf, bad_in_buf;
    uint32_t bad_run;             /* consecutive bad blocks from first_fail */
    uint32_t fill_run;            /* ...of which are all-0x08 */
    uint32_t good_above;          /* decodable blocks in [nblocks, +ABOVE) */
    uint32_t hdr_below1, hdr_last_good, hdr_first_bad, hdr_end;
    /* Byte-exact, relative to ba, because block granularity cannot say
     * whether the written data ends ON a block boundary. If it does not, the
     * producer is not counting in blocks and neither should the theory. */
    int64_t  fill_begin, fill_end;
    uint32_t before[4];           /* the four dwords immediately below ba */
    /* What the GUEST declared, as opposed to what we read back out of the
     * voice register file. */
    uint32_t guest_ebo, guest_ba;
    uint32_t guest_ebo_sets, guest_ba_sets;
    /* NOT YET EXERCISED BY A RUN -- added with the rest of the record but
     * after the only session that had a scene with ADPCM in it, so nothing
     * has printed these. They are here because the one thing the measurement
     * could not settle is why the declared buffer is ELEVEN blocks longer
     * than the audio, and a loop that wraps somewhere other than ebo would
     * answer it: lbo == first_fail * ADPCM_SAMPLES_PER_BLOCK would mean the
     * data end is in LBO and this model is wrapping on the wrong field. */
    uint32_t lbo, cbo_at_fail;
    uint8_t  loop, persist, spb;
} AdpcmExtent;

/* THE GUEST'S OWN DECLARATION, RECORDED WHERE IT ARRIVES.
 *
 * ebo and ba are read back out of the voice register file, which lives in
 * guest RAM -- so a read-back cannot distinguish "the guest set this for this
 * buffer" from "this is left over from the last thing that used this voice".
 * The PIO methods can: NV1BA0_PIO_SET_VOICE_CFG_BUF_BASE and
 * ..._BUF_EBO are the only two ways either value is supposed to arrive.
 *
 * A set count of ZERO on a voice that is decoding is the interesting reading,
 * and it is the one a read-back can never produce: it would mean the length
 * the fetch is using was never declared for this buffer at all. Which is why
 * the counts are printed beside the values and not instead of them.
 *
 * Two stores on a path that already does a masked read-modify-write into
 * guest RAM, so unconditional -- the same standing as g_apu_adpcm_ok. Only
 * the printing is behind the switch. */
uint32_t g_voice_guest_ebo[MCPX_HW_MAX_VOICES];
uint32_t g_voice_guest_ba[MCPX_HW_MAX_VOICES];
uint32_t g_voice_guest_ebo_sets[MCPX_HW_MAX_VOICES];
uint32_t g_voice_guest_ba_sets[MCPX_HW_MAX_VOICES];

static AdpcmExtent g_adpcm_extent[ADPCM_EXTENT_SLOTS];
static unsigned g_adpcm_extent_n;
unsigned long g_adpcm_extent_scans;

static int adpcm_extent_on(void)
{
    static int on = -1;
    if (on < 0) on = recomp_switch_on("RECOMP_APU_ADPCM_EXTENT");
    return on;
}

/* See the note over adpcm_decode_block in apu_state.h. Set once per voice
 * from the frame thread, read by the decoder, so the pure function stays
 * getenv-free. */
int g_adpcm_hw_header;
unsigned long g_adpcm_hw_header_accepted;

static int adpcm_hw_header_on(void)
{
    static int on = -1;
    if (on < 0) on = recomp_switch_on("RECOMP_APU_ADPCM_HW_HEADER");
    return on;
}

/* adpcm_decode_block's refusal test, and only that: it returns 0 when the
 * fourth header byte of any channel is non-zero or its step index is outside
 * 0..88. Anything this accepts, the decoder accepts. */
static int adpcm_hdr_ok(hwaddr addr, unsigned channels)
{
    unsigned ch;
    for (ch = 0; ch < channels; ++ch) {
        uint32_t w = (uint32_t)ldl_le_phys(address_space_memory,
                                           addr + (hwaddr)ch * 4);
        if ((w >> 24) != 0) return 0;
        if (((w >> 16) & 0xFF) > 88) return 0;
    }
    return 1;
}

static int adpcm_all_08(hwaddr addr, unsigned channels)
{
    unsigned ch;
    for (ch = 0; ch < channels; ++ch)
        if (ldl_le_phys(address_space_memory, addr + (hwaddr)ch * 4)
                != 0x08080808u)
            return 0;
    return 1;
}

/* The address the fetch would use for block k, with k allowed to go negative
 * so the region below the base can be probed. Deliberately the same
 * translation the fetch uses. */
static hwaddr adpcm_block_addr(MCPXAPUState *d, int stream, hwaddr seg,
                               uint32_t ba, size_t bs, int32_t k)
{
    int64_t lin = (int64_t)k * (int64_t)bs;
    if (stream) return (hwaddr)((int64_t)seg + lin);
    lin += (int64_t)ba;
    if (lin < 0) return 0;
    return get_data_ptr(d->regs[NV_PAPU_VPSGEADDR], 0xFFFFFFFFu,
                        (uint32_t)lin);
}

static void adpcm_extent_scan(MCPXAPUState *d, uint32_t v, int stream,
                              unsigned channels, size_t bs, uint32_t ba,
                              uint32_t ebo, hwaddr seg, uint32_t nblocks,
                              uint32_t first_fail, uint32_t lbo, uint32_t cbo,
                              int loop, int persist, unsigned spb)
{
    AdpcmExtent *e;
    int32_t k;
    hwaddr a;

    if (!adpcm_extent_on()) return;
    if (g_adpcm_extent_n >= ADPCM_EXTENT_SLOTS) return;
    for (unsigned i = 0; i < g_adpcm_extent_n; ++i)
        if (g_adpcm_extent[i].v == (uint16_t)v) return;   /* once per voice */

    e = &g_adpcm_extent[g_adpcm_extent_n++];
    e->v = (uint16_t)v;
    e->used = 1;
    e->stream = (uint8_t)(stream ? 1 : 0);
    e->channels = (uint8_t)channels;
    e->bs = (uint32_t)bs;
    e->ba = ba;
    e->ebo = ebo;
    e->seg = (uint32_t)seg;
    e->nblocks = nblocks;
    e->first_fail = first_fail;
    e->lbo = lbo;
    e->cbo_at_fail = cbo;
    e->loop = (uint8_t)(loop ? 1 : 0);
    e->persist = (uint8_t)(persist ? 1 : 0);
    e->spb = (uint8_t)spb;
    if (v < MCPX_HW_MAX_VOICES) {
        e->guest_ebo = g_voice_guest_ebo[v];
        e->guest_ba = g_voice_guest_ba[v];
        e->guest_ebo_sets = g_voice_guest_ebo_sets[v];
        e->guest_ba_sets = g_voice_guest_ba_sets[v];
    }
    ++g_adpcm_extent_scans;

    /* Below the base. THE discriminator: real ADPCM here means the buffer
     * starts lower than ba says. */
    for (k = -1; k >= -(int32_t)ADPCM_EXTENT_BELOW; --k) {
        if (!stream && (int64_t)ba + (int64_t)k * (int64_t)bs < 0) break;
        a = adpcm_block_addr(d, stream, seg, ba, bs, k);
        if (!adpcm_hdr_ok(a, channels)) break;
        ++e->good_below;
    }
    a = adpcm_block_addr(d, stream, seg, ba, bs, -1);
    e->hdr_below1 = (uint32_t)ldl_le_phys(address_space_memory, a);

    /* Inside the buffer, as the model believes it to be. */
    for (k = 0; k < (int32_t)nblocks && k < (int32_t)(nblocks + 1); ++k) {
        a = adpcm_block_addr(d, stream, seg, ba, bs, k);
        if (adpcm_hdr_ok(a, channels)) ++e->good_in_buf;
        else ++e->bad_in_buf;
    }
    if (first_fail > 0) {
        a = adpcm_block_addr(d, stream, seg, ba, bs,
                             (int32_t)first_fail - 1);
        e->hdr_last_good = (uint32_t)ldl_le_phys(address_space_memory, a);
    }
    a = adpcm_block_addr(d, stream, seg, ba, bs, (int32_t)first_fail);
    e->hdr_first_bad = (uint32_t)ldl_le_phys(address_space_memory, a);
    a = adpcm_block_addr(d, stream, seg, ba, bs, (int32_t)nblocks);
    e->hdr_end = (uint32_t)ldl_le_phys(address_space_memory, a);

    /* How far the refusal runs, and how much of it is the 0x08 fill. */
    for (k = (int32_t)first_fail;
         k < (int32_t)(nblocks + ADPCM_EXTENT_ABOVE); ++k) {
        a = adpcm_block_addr(d, stream, seg, ba, bs, k);
        if (adpcm_hdr_ok(a, channels)) break;
        ++e->bad_run;
        if (adpcm_all_08(a, channels)) ++e->fill_run;
    }
    for (k = (int32_t)nblocks;
         k < (int32_t)(nblocks + ADPCM_EXTENT_ABOVE); ++k) {
        a = adpcm_block_addr(d, stream, seg, ba, bs, k);
        if (adpcm_hdr_ok(a, channels)) ++e->good_above;
    }

    /* THE FILL, TO THE BYTE. A dword at a time through the same translation,
     * from four blocks before the first refusal to ADPCM_EXTENT_ABOVE blocks
     * past the nominal end. Both offsets are relative to ba, so fill_begin is
     * literally "how many bytes of this buffer were written". */
    e->fill_begin = -1;
    e->fill_end = -1;
    {
        int64_t lo = ((int64_t)first_fail - 4) * (int64_t)bs;
        int64_t hi = ((int64_t)nblocks + ADPCM_EXTENT_ABOVE) * (int64_t)bs;
        int64_t off;
        if (lo < 0) lo = 0;
        for (off = lo; off + 4 <= hi; off += 4) {
            hwaddr w = stream ? (hwaddr)((int64_t)seg + off)
                              : get_data_ptr(d->regs[NV_PAPU_VPSGEADDR],
                                             0xFFFFFFFFu,
                                             (uint32_t)((int64_t)ba + off));
            uint32_t v32 = (uint32_t)ldl_le_phys(address_space_memory, w);
            if (e->fill_begin < 0) {
                if (v32 == 0x08080808u) e->fill_begin = off;
            } else if (v32 != 0x08080808u) {
                e->fill_end = off;
                break;
            }
        }
    }
    for (k = 0; k < 4; ++k) {
        int64_t off = -(int64_t)(4 - k) * 4;
        hwaddr w;
        if (!stream && (int64_t)ba + off < 0) { e->before[k] = 0; continue; }
        w = stream ? (hwaddr)((int64_t)seg + off)
                   : get_data_ptr(d->regs[NV_PAPU_VPSGEADDR], 0xFFFFFFFFu,
                                  (uint32_t)((int64_t)ba + off));
        e->before[k] = (uint32_t)ldl_le_phys(address_space_memory, w);
    }
}

void mcpx_apu_adpcm_extent_report(void)
{
    unsigned i;

    if (!adpcm_extent_on()) return;
    fprintf(stderr, "  [APU-ADPCM-EXTENT] %u voice(s) scanned%s\n",
            g_adpcm_extent_n,
            g_adpcm_extent_n ? "" : "  <- NOTHING SCANNED: no ADPCM voice"
                                    " refused a block, so this says nothing");
    for (i = 0; i < g_adpcm_extent_n; ++i) {
        const AdpcmExtent *e = &g_adpcm_extent[i];
        fprintf(stderr, "  [APU-ADPCM-EXTENT]   v%u%s%s bs=%u ba=%u ebo=%u"
                " nblocks=%u first_fail=%u gap=%d\n",
                e->v, e->stream ? " S" : "", e->channels == 2 ? " st" : "",
                e->bs, e->ba, e->ebo, e->nblocks, e->first_fail,
                (int)e->nblocks - (int)e->first_fail);
        fprintf(stderr, "  [APU-ADPCM-EXTENT]     below base: %u decodable"
                " block(s) (>=11 means THE BASE IS TOO HIGH; 0 means the"
                " buffer is genuinely short), hdr[-1]=%08X\n",
                e->good_below, e->hdr_below1);
        fprintf(stderr, "  [APU-ADPCM-EXTENT]     in buffer: %u good %u bad"
                " | refusal runs %u block(s), %u of them all-0x08"
                " | %u decodable past the end\n",
                e->good_in_buf, e->bad_in_buf, e->bad_run, e->fill_run,
                e->good_above);
        fprintf(stderr, "  [APU-ADPCM-EXTENT]     hdr last_good=%08X"
                " first_bad=%08X at_nblocks=%08X | before ba:"
                " %08X %08X %08X %08X\n",
                e->hdr_last_good, e->hdr_first_bad, e->hdr_end,
                e->before[0], e->before[1], e->before[2], e->before[3]);
        fprintf(stderr, "  [APU-ADPCM-EXTENT]     the 0x08 fill runs"
                " [%lld,%lld) bytes from ba; the buffer is %u bytes and"
                " %u block(s) of it were written%s\n",
                (long long)e->fill_begin, (long long)e->fill_end,
                e->nblocks * e->bs,
                e->bs ? (unsigned)(e->fill_begin > 0
                                   ? e->fill_begin / e->bs : 0) : 0,
                (e->fill_begin >= 0 && e->bs
                 && (uint32_t)e->fill_begin % e->bs)
                    ? "  <- NOT on a block boundary" : "");
        fprintf(stderr, "  [APU-ADPCM-EXTENT]     the guest DECLARED"
                " ebo=%u (%u set%s) ba=%u (%u set%s)%s%s\n",
                e->guest_ebo, e->guest_ebo_sets,
                e->guest_ebo_sets == 1 ? "" : "s",
                e->guest_ba, e->guest_ba_sets,
                e->guest_ba_sets == 1 ? "" : "s",
                e->guest_ebo_sets == 0
                    ? "  <- EBO WAS NEVER DECLARED FOR THIS VOICE: the length"
                      " the fetch used is left over in the voice register file"
                    : "",
                (e->guest_ebo_sets && e->guest_ebo != e->ebo)
                    ? "  <- and it is NOT what the fetch read back" : "");
        fprintf(stderr, "  [APU-ADPCM-EXTENT]     loop=%u persist=%u spb=%u"
                " lbo=%u cbo=%u%s\n",
                e->loop, e->persist, e->spb, e->lbo, e->cbo_at_fail,
                (e->lbo && e->lbo == e->first_fail * ADPCM_SAMPLES_PER_BLOCK)
                    ? "  <- LBO IS THE DATA END: the wrap point is in LBO and"
                      " this model wraps on EBO"
                    : "");
    }
    fflush(stderr);
}

/* OFF by default. RECOMP_APU_ADPCM_GUARD=1 substitutes silence for the
 * uninitialised tail of a refused or short block. Off, the default path is
 * byte-identical to what it was, so the counters above measure the bug rather
 * than the fix -- which also means an ADPCM voice keeps reading uninitialised
 * stack until somebody turns this on. Both halves of that are deliberate: this
 * repository has shipped one APU guard on an argument and made a crash worse,
 * and the rule that came out of it is a run count, not a better argument. */
/* RECOMP_APU_SEGMENT_SPB, default OFF -- BUILT, MEASURED, AND IT IS NOT THE
 * ADPCM FIX. Scene-matched gameplay A/B, 16 Sep 2026:
 *     off  ok=697869 fail=27801  (3.83%)
 *     on   ok=584716 fail=25224  (4.13%)
 * i.e. no effect, and [APU-ADPCM-SPB] printed in NEITHER arm, which means
 * g_adpcm_spb_voice was zero: this scene contains no streaming ADPCM voice at
 * all, so the switch could not have done anything. The failing voices are
 * non-stream (the ring reads "v4 blk 161/170", no S flag), and for those the
 * segment descriptor is never consulted. THE ADPCM DEFECT IS STILL OPEN and
 * the first block of every buffer still always decodes while later ones fail,
 * so a stride or base-address error remains the shape to look for -- just not
 * this one. Default OFF because it is an unmeasured behaviour change for any
 * scene that DOES stream ADPCM, and default-on would ship that to a player on
 * the strength of an argument.
 *
 * THE BLOCK STRIDE OF A STREAMING VOICE COMES FROM ITS SEGMENT, NOT FROM ITS
 * VOICE REGISTER. The SSL segment descriptor packs samples-per-block at bits
 * 18-22 beside the container size at 16-17, and this model already lets the
 * segment override the container size (container_size_index = seg_cs, and the
 * ADPCM case then forces sample_size) -- but it decoded seg_spb into a local
 * and NEVER READ IT, so block_size stayed 36 * the VOICE register's
 * samples-per-block. When the two disagree every block after the first is
 * fetched at the wrong offset.
 *
 * That is exactly the measured shape. Player session 16 Sep 2026 18:33,
 * 36,429 decode failures in 170 s:
 *     [APU-ADPCM-FAIL] first=0 last=1116 mid=44775
 *     last 8: v11 blk 213/214 hdr 08080808   (the same block, over and over)
 * first=0 is the whole finding: block 0 starts at offset 0 whatever the
 * stride is, so it always decodes, and every later block is displaced by a
 * multiple of the error. 0x08080808 is not a header at all -- byte 3 must be
 * zero and is 8 -- it is ADPCM sample data being read as one.
 *
 * Each failure emits an UNINITIALISED stack array as audio at full scale
 * (adpcm_decoded is automatic and the caller reads it regardless), so this is
 * not only silence, it is noise.
 *
 * =0 restores the old stride for an A/B. The [APU-ADPCM] line prints which
 * one ran. */
int mcpx_apu_segment_spb(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("RECOMP_APU_SEGMENT_SPB");
        on = (e && *e) ? (atoi(e) != 0) : 0;
    }
    return on;
}
unsigned long g_adpcm_spb_voice, g_adpcm_spb_seg, g_adpcm_spb_differ;

int mcpx_apu_adpcm_guard(void)
{
    /* DEFAULT ON since 16 Sep 2026 (night). A refused block used to leave
     * adpcm_decoded -- an UNINITIALISED automatic array -- for the caller to
     * read, so every failure handed the mixer stack memory at full scale.
     * 1,387 refusals in one 200 s run, 27,801 in a 150 s gameplay run.
     *
     * The CAUSE IS STILL OPEN and two theories died getting here: the
     * segment descriptor's samples-per-block (measured, no effect, and the
     * scene has no streaming ADPCM voice), and a page-table translation going
     * wrong past the first page (measured: page0=0 pagehi=1387 looked like a
     * fit, but the returned page address is properly 4 KB-aligned and
     * plausible, and the lowest failing block per voice is 208 of 219 and 275
     * -- neither a multiple of the 113 blocks that fit in a page). What the
     * probe DID establish is that failures cluster in the LAST ~5% of a
     * buffer: v0 fails from block 208 of 219 onward. That is the shape of
     * reading past the data the guest actually wrote, not of a broken
     * translation.
     *
     * Turning this on does not fix that. It makes the symptom silence
     * instead of noise, which is the right default while the cause is open:
     * emitting uninitialised memory as audio is not a thing to ship. =0
     * restores the old behaviour for anyone measuring the cause. */
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("RECOMP_APU_ADPCM_GUARD");
        on = (e && *e) ? (atoi(e) != 0) : 1;
    }
    return on;
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
        /* The segment's own samples-per-block, which decides the block
         * stride below. Counted in both arms so a run says whether the two
         * sources ever disagreed -- if differ=0 this switch changed nothing
         * and the ADPCM failures are something else. */
        ++g_adpcm_spb_voice;
        if ((unsigned)(seg_spb + 1) != samples_per_block) ++g_adpcm_spb_differ;
        if (mcpx_apu_segment_spb()) {
            samples_per_block = (unsigned)(seg_spb + 1);
            ++g_adpcm_spb_seg;
        }
        assert(seg_len > 0);
        ebo = seg_len - 1;
    }

    bool adpcm =
        (container_size_index == NV_PAVS_VOICE_CFG_FMT_CONTAINER_SIZE_ADPCM);

    if (adpcm) {
        block_size = 36;
        /* Cached accessor; this is the frame thread, not the trap handler. */
        g_adpcm_hw_header = adpcm_hw_header_on();
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
                /* One 36-byte ADPCM block per channel is the most that either
                 * adpcm_block (72 bytes) or adpcm_decoded (65 frames per
                 * channel) can hold, and the most the reader below can
                 * address. Anything longer is a stack overrun in both
                 * directions; see the g_apu_adpcm_oversize comment. */
                size_t fetch = block_size;
                if (fetch > (size_t)36 * channels) {
                    fetch = (size_t)36 * channels;
                    g_apu_adpcm_oversize++;
                }
                if (stream) {
                    hwaddr addr = segment_offset + linear_addr;
                    memcpy(adpcm_block, &d->ram_ptr[addr & g_apu_ram_mask],
                           fetch);
                } else {
                    linear_addr += ba;
                    for (unsigned int word_index = 0;
                         word_index < fetch / 4; word_index++) {
                        hwaddr addr = get_data_ptr(d->regs[NV_PAPU_VPSGEADDR],
                                                   0xFFFFFFFF, linear_addr);
                        adpcm_block[word_index] =
                            ldl_le_phys(address_space_memory, addr);
                        linear_addr += 4;
                    }
                }
                /* THE RETURN VALUE, WHICH WAS DISCARDED. Frames per channel,
                 * 0 for a block the decoder refused -- and a refusal can
                 * happen after it has already written the first sample, so
                 * "0" does not mean "wrote nothing". */
                int decoded = adpcm_decode_block(adpcm_decoded,
                                                 (uint8_t *)adpcm_block,
                                                 fetch, channels);
                if (decoded <= 0) {
                    g_apu_adpcm_fail++;
                    /* WHICH BLOCKS FAIL. 15,529 refusals in one player
                     * session (1.8% of blocks), every one emitting stack
                     * memory as audio, and nothing recorded which voice,
                     * which block, or what the header held. The decoder
                     * refuses on header byte 3 != 0 or a step index > 88,
                     * i.e. the block is misaligned or is not ADPCM. first/
                     * last/mid says whether it is the partial last block of
                     * every buffer or something else; the ring of 8 shows
                     * the header bytes. Printed by the report. */
                    {
                        unsigned slot = (unsigned)(g_adpcm_fail_ring & 7u);
                        const uint8_t *hb = (const uint8_t *)adpcm_block;
                        uint32_t nblocks = ebo / ADPCM_SAMPLES_PER_BLOCK + 1;
                        g_adpcm_fail_v[slot] = (uint16_t)v;
                        g_adpcm_fail_stream[slot] = (uint8_t)(stream ? 1 : 0);
                        g_adpcm_fail_ch[slot] = (uint8_t)channels;
                        g_adpcm_fail_block[slot] = block_index;
                        g_adpcm_fail_nblocks[slot] = nblocks;
                        g_adpcm_fail_bs[slot] = (uint32_t)block_size;
                        g_adpcm_fail_ba[slot] = stream ? 0u : ba;
                        g_adpcm_fail_ebo[slot] = ebo;
                        g_adpcm_fail_hdr[slot] = (uint32_t)hb[0] | ((uint32_t)hb[1] << 8)
                                               | ((uint32_t)hb[2] << 16) | ((uint32_t)hb[3] << 24);
                        {
                            uint32_t la = block_index * (uint32_t)block_size
                                        + (stream ? 0u : ba);
                            uint32_t pg = la / TARGET_PAGE_SIZE;
                            g_adpcm_fail_lin[slot] = la;
                            g_adpcm_fail_page[slot] = pg;
                            g_adpcm_fail_prd[slot] = stream ? 0u
                                : (uint32_t)ldl_le_phys(address_space_memory,
                                      d->regs[NV_PAPU_VPSGEADDR] + pg * 4 * 2);
                            if (pg == 0) ++g_adpcm_fail_page0;
                            else ++g_adpcm_fail_pagehi;
                            if (v < MCPX_HW_MAX_VOICES
                                && (!g_adpcm_first_fail_blk[v]
                                    || block_index < g_adpcm_first_fail_blk[v])) {
                                g_adpcm_first_fail_blk[v] = block_index;
                                g_adpcm_fail_nblocks_v[v] = nblocks;
                            }
                            /* Once per voice, on its first refusal, and only
                             * with RECOMP_APU_ADPCM_EXTENT set. */
                            adpcm_extent_scan(d, v, stream ? 1 : 0, channels,
                                              block_size, ba, ebo,
                                              segment_offset, nblocks,
                                              block_index, lbo, cbo,
                                              loop, persist,
                                              samples_per_block);
                        }
                        g_adpcm_fail_ring++;
                        if (block_index == 0) g_adpcm_fail_first++;
                        else if (block_index + 1 >= nblocks) g_adpcm_fail_last++;
                        else g_adpcm_fail_mid++;
                    }
                    if (mcpx_apu_adpcm_guard()) {
                        memset(adpcm_decoded, 0, sizeof(adpcm_decoded));
                        g_apu_adpcm_silenced++;
                    }
                } else {
                    g_apu_adpcm_ok++;
                    if (decoded < (int)ADPCM_SAMPLES_PER_BLOCK) {
                        g_apu_adpcm_short++;
                        if (mcpx_apu_adpcm_guard()) {
                            size_t got = (size_t)decoded * channels;
                            memset(&adpcm_decoded[got], 0,
                                   sizeof(adpcm_decoded)
                                   - got * sizeof adpcm_decoded[0]);
                            g_apu_adpcm_silenced++;
                        }
                    }
                }
                /* Still cached unconditionally, because a refused block is
                 * refused every time it is fetched and retrying it 63 more
                 * times this frame would only cost CPU. What has changed is
                 * that the cached result is now counted, and optionally
                 * silence rather than stack. */
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
        while (rs->phase >= 1.0f) {
            /* SNAPSHOT, because the guest thread can zero ncarry under us.
             * voice_reset_filters -> voice_resample_reset runs from VOICE_ON on
             * a guest thread while this runs on the frame thread. Re-reading
             * rs->ncarry between the test and the length computation meant a
             * reset landing in that gap made (size_t)(0 - 1) == SIZE_MAX the
             * memmove length: a host SEGV, in a subsystem that already has an
             * unexplained crash. The window is narrow and may never have
             * fired; it costs one local to close. */
            int n = rs->ncarry;
            if (n <= 0) break;
            rs->phase -= 1.0f;
            memmove(&rs->carry[0], &rs->carry[1],
                    (size_t)(n - 1) * sizeof rs->carry[0]);
            rs->ncarry = n - 1;
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

/* How many per-voice rows the report prints, of MCPX_HW_MAX_VOICES.
 *
 * THIS CAP IS WHY THE TABLE LIED FOR A WEEK, so it is a switch now and it
 * reports what it suppressed. The rows are chosen by scanning v upward and
 * taking the first `rows` voices with frames != 0 -- and `frames` is
 * CUMULATIVE for the life of the process, never reset. So once `rows` voices
 * have each been processed even once, those same voices hold every slot for
 * the rest of the run and no higher-numbered voice can ever appear again.
 *
 * The 19 Sep 2026 black-screen run reports on=160 voices started, of 256, and
 * printed twelve -- voices 0-11, from the first report to the last. Voices
 * 64-67 appear only in the early reports and were read as "retired"; they were
 * not, they were crowded out. Eleven of the twelve shown do stop being
 * processed, at staggered times, and that is real -- but "the music stopped"
 * was never measurable here, because the music could have moved to any of the
 * other 244 rows the table had already become incapable of printing.
 *
 * Default 12 so logs stay comparable with every run taken before this. Raise
 * it when the question is about the pool rather than about voices 0-11. */
#define VOICE_RATE_ROWS_DEFAULT 12u

static unsigned voice_rate_rows(void)
{
    static unsigned rows;
    if (!rows) {
        const char *e = getenv("RECOMP_VOICE_RATES_ROWS");
        long n = e ? strtol(e, NULL, 0) : 0;
        if (n <= 0) n = VOICE_RATE_ROWS_DEFAULT;
        if (n > MCPX_HW_MAX_VOICES) n = MCPX_HW_MAX_VOICES;
        rows = (unsigned)n;
    }
    return rows;
}

void mcpx_apu_voice_rate_report(void)
{
    unsigned v, shown = 0, any = 0, eligible = 0, hidden_2d = 0, seen = 0;
    const unsigned rows = voice_rate_rows();
    unsigned long tot = 0, tot_off = 0;
    if (!voice_rate_on()) return;
    g_voice_fresh_window_s = voice_fresh_window_s();
    for (v = 0; v < MCPX_HW_MAX_VOICES; v++) {
        tot += g_voice_rate[v].frames;
        tot_off += g_voice_rate[v].off_frames;
        if (g_voice_rate[v].off_frames) any++;
        if (g_voice_rate[v].frames) {
            eligible++;
            /* Would this row be reached before the budget ran out? The print
             * loop below scans the same order, so counting it here is the
             * same selection. */
            if (seen++ >= rows && v >= MCPX_HW_MAX_3D_VOICES) hidden_2d++;
        }
    }
    /* eligible/rows is the positive control the table never had. A row that is
     * absent because its voice never ran and a row that is absent because
     * twelve lower-numbered voices got there first read identically, and the
     * second kind is the one that misleads. Say which. */
    fprintf(stderr, "  [VOICE-RATE] %lu voice-frames, %lu needing resampling "
            "(%.2f%%), %u voice(s) affected%s\n",
            tot, tot_off, tot ? 100.0 * (double)tot_off / (double)tot : 0.0, any,
            tot ? "" : "   <- NO VOICE FRAMES: nothing is being processed");
    fprintf(stderr, "  [VOICE-RATE] showing %u of %u voice(s) with frames, of"
            " %u in the pool%s\n",
            eligible < rows ? eligible : rows, eligible,
            (unsigned)MCPX_HW_MAX_VOICES,
            eligible > rows
                ? "   <- ROWS SUPPRESSED: rows are the LOWEST-numbered voices"
                  " ever processed, not the active ones."
                  " RECOMP_VOICE_RATES_ROWS raises the cap"
                : "");
    /* AND NAME IT WHEN THE HIDDEN ROWS ARE THE 2D BIN, because that is the
     * music and that is what G1 is about. 2D voices are handles at or above
     * MCPX_HW_MAX_3D_VOICES (64) and this table scans upward from 0, so every
     * 3D voice that has ever run takes a slot ahead of them. In a session that
     * has touched all 64, nothing below RECOMP_VOICE_RATES_ROWS=65 can show a
     * single 2D voice -- and "the music is voices 64-67" was read off an early
     * report, before the 3D voices had crowded them out. */
    /* THE STALE-BUFFER CENSUS, ACROSS THE WHOLE POOL.
     *
     * The per-voice [VOICE-FRESH] line already prints its own verdict, "the
     * guest is not refilling this buffer". In the 19 Sep 2026 black-screen
     * run it printed that 457 times and nobody read it, because it is one
     * line per voice per report, buried among eleven others, and there was
     * no total anywhere.
     *
     * What that run actually says, read voice by voice afterwards: voice 68
     * is 4.3% stale and every other producing voice is 99.3% to 99.9%, with
     * voice 3 replaying one 32-sample slot for 65,933 ms. Voice 68 is the
     * positive control and it is why the rest cannot be dismissed as a dead
     * instrument -- the guest demonstrably CAN refill a buffer, and for
     * everything else it stopped.
     *
     * So the census is printed as one line, over all 256 voices rather than
     * the dozen the table shows, and it names the healthiest voice beside
     * the count so the control travels with the finding. */
    if (voice_fresh_on()) {
        unsigned stale_voices = 0, classified_voices = 0, best_v = 0;
        double worst_ms = 0.0, best_pct = 101.0;
        for (v = 0; v < MCPX_HW_MAX_VOICES; v++) {
            const VoiceRate *r = &g_voice_rate[v];
            unsigned long cl = r->fresh_slots + r->stale_slots;
            double pct;
            if (!cl) continue;
            ++classified_voices;
            pct = 100.0 * (double)r->stale_slots / (double)cl;
            if (pct >= 90.0) {
                ++stale_voices;
                if (r->max_stale_run * NUM_SAMPLES_PER_FRAME / 48.0 > worst_ms)
                    worst_ms = r->max_stale_run * NUM_SAMPLES_PER_FRAME / 48.0;
            }
            if (pct < best_pct) { best_pct = pct; best_v = v; }
        }
        if (classified_voices)
            fprintf(stderr, "  [VOICE-FRESH] census: %u of %u voice(s) are"
                    " >=90%% STALE, worst replay %.0f ms | freshest is"
                    " voice %u at %.1f%% stale%s\n",
                    stale_voices, classified_voices, worst_ms,
                    best_v, best_pct,
                    (stale_voices && best_pct < 50.0)
                        ? "   <- that one IS being refilled, so the rest are"
                          " not an instrument failure"
                        : (stale_voices == classified_voices
                            ? "   <- NO healthy voice to compare against:"
                              " cannot tell a stalled guest from a dead"
                              " instrument" : ""));
    }

    if (hidden_2d)
        fprintf(stderr, "  [VOICE-RATE] %u of the hidden voice(s) are 2D"
                " (handle >= %u) -- THAT IS THE BIN THE MUSIC IS IN. Set"
                " RECOMP_VOICE_RATES_ROWS=%u to see the whole pool.\n",
                hidden_2d, (unsigned)MCPX_HW_MAX_3D_VOICES,
                (unsigned)MCPX_HW_MAX_VOICES);
    for (v = 0; v < MCPX_HW_MAX_VOICES && shown < rows; v++) {
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

    /* DO NOT ROUTE INTO AN HRTF SUBMIX WHEN NO HRTF RUNS.
     *
     * Every voice below MCPX_HW_MAX_3D_VOICES -- i.e. every positional voice,
     * i.e. every sound effect -- had its first four bin assignments replaced by
     * d->vp.hrtf_submix[], discarding the routing the guest asked for. That is
     * correct only if something downstream then performs HRTF and mixes those
     * submixes down. Nothing does: .hrtf is false (apu_shim.h), and the GP and
     * EP are stubs, so mcpx_apu_dsp_frame reads mixbins[0] and mixbins[1] and
     * throws the other thirty away.
     *
     * MEASURED, one 200 s gameplay run, with the positive control moving:
     *
     *     [APU-BIN] 2D heard=380614 lost=0 | 3D heard=0 lost=375382
     *               hrtf_submix=6,8,7,9 | lost by bin: 6,7,8,9,10
     *
     * Music is on 2D voices and lands in bins 0 and 1: 380,614 heard, none
     * lost. Sound effects are the 3D voices: not one heard, 375,382 voice
     * frames discarded, into bins 6 to 10. The samples were produced
     * correctly and then dropped on the floor -- which is why every
     * instrument upstream of here read healthy while the player heard no
     * effects at all.
     *
     * Nothing could see it: dbg->bin[i] below is written and read by nothing,
     * and no counter anywhere recorded which bin a voice landed in. Thirty of
     * thirty-two bins were discarded every frame in silence.
     *
     * The override is inherited from upstream verbatim. Gating it is one line
     * and restores the guest's own V0BIN..V3BIN, which is what the hardware
     * would use with HRTF disabled. RECOMP_APU_HRTF_BINS=1 restores the old
     * behaviour for an A/B. */
    if (v < MCPX_HW_MAX_3D_VOICES && mcpx_apu_hrtf_bins()) {
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
        if ((v < MCPX_HW_MAX_3D_VOICES) && (b < 4) && mcpx_apu_hrtf_bins()) {
            /* Only when the HRTF submix override is applied: with it off,
             * bin[b] is the guest's own V0BIN..V3BIN and takes that bin's
             * submix headroom like every other route. Gating this on the
             * same switch also makes the [APU-BIN] hrtf= field mean the
             * override, so an A/B on RECOMP_APU_HRTF_BINS can tell its arms
             * apart -- before, it read "on" in both. */
            g_bin_hrtf_on = 1;
            hr = (float)(1 << d->vp.hrtf_headroom);
        } else {
            hr = (float)(1 << d->vp.submix_headroom[bin[b]]);
        }
        g *= attenuate(vol[b]) / hr;
        /* WHICH BIN, AND DOES ANYTHING READ IT.
         *
         * apu_dsp.c reads mixbins[0] and mixbins[1] and nothing else -- the GP
         * and EP are stubs -- so thirty of the thirty-two bins are discarded
         * every frame with no counter anywhere. And every voice below
         * MCPX_HW_MAX_3D_VOICES has bin[0..3] overwritten with hrtf_submix[]
         * a few lines above, unconditionally, even though .hrtf is false and
         * the HRTF path never runs. Voices 0-63 are the positional ones, i.e.
         * the sound effects; music sits on 2D voices in the 68-70 range and
         * survives. That is the whole shape of "music plays, effects do not",
         * and until now nothing could see it: the per-voice bin assignment is
         * written to a debug struct that no code reads.
         *
         * heard_2d is the positive control. Music is audible, so it MUST move;
         * if it reads zero the instrument is dead and lost_3d proves nothing.
         *
         * "AUDIBLE" MEANS "A BIN THE MIXDOWN SUMS", AND THAT IS NOW A SWITCH.
         * With RECOMP_APU_MIXDOWN_ALL on, apu_dsp.c sums all 32 bins, so a
         * voice routed to bin 6 is heard. This test used to hard-code bins 0
         * and 1, so the first run after the widening reported 3D heard=0
         * lost=343968 with the effects playing -- the instrument was scoring
         * the old mixdown. It has to ask the mixdown what it reads. */
        {
            int audible = mcpx_apu_mixdown_all()
                        || (bin[b] == 0 || bin[b] == 1);
            int is3d = (v < MCPX_HW_MAX_3D_VOICES);
            float amp = 0.0f;
            for (int i = 0; i < NUM_SAMPLES_PER_FRAME; i++) {
                float x = g * samples[i][b % channels];
                if (x > amp) amp = x; else if (-x > amp) amp = -x;
            }
            if (amp > 0.0f) {
                if (is3d) { if (audible) ++g_bin_heard_3d; else ++g_bin_lost_3d; }
                else      { if (audible) ++g_bin_heard_2d; else ++g_bin_lost_2d; }
                if (!audible && bin[b] < 32) ++g_bin_lost_hist[bin[b]];
            }
        }
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

        /* The walk is driven by a LOCAL cursor, not by d->regs[current].
         *
         * d->regs[current] is how the guest learns which voice idled, so it has
         * to stay pointing at that voice until the trap is serviced -- which is
         * why the original returned outright. But freezing the guest-visible
         * register and abandoning the rest of the mix are two different things,
         * and only the first is required. With the local cursor the register
         * can be held still while every voice behind the dead one still gets
         * rendered. See mcpx_apu_se_while_trapped. */
        uint16_t cur = (uint16_t)d->regs[top];
        int trap_held = 0;
        /* Which voice's link field pointed at the one we are looking at.
         * 0xFFFF means "straight off TVL", i.e. it is the list head. The two
         * cases want different fixes -- a bad head register and a bad link are
         * written by different code -- and only this tells them apart. */
        uint16_t came_from = 0xFFFF;

        /* CVL AND NVL ARE ONE CURSOR, AND BOTH HAVE TO HOLD STILL.
         *
         * Hardware stalls the walk on a trap with CVL naming the idle voice and
         * NVL naming its successor, and the guest reads BOTH: RemoveIdleVoice
         * repairs the pair to steer an in-progress walk through the voice it is
         * taking out. A pair that half-moves is worse than one that does not
         * move at all, because CVL still looks right.
         *
         * Holding only CVL was enough while the frame thread idled on TRAPPED:
         * nothing then ran to move NVL either. That was xemu's behaviour and
         * this model's until RECOMP_APU_SE_WHILE_TRAPPED became the default on
         * 15 Sep 2026. With the engine now rendering through a trap this
         * function is re-entered every 1500th of a second while the guest still
         * owes a service, and it used to rewrite NVL at every voice behind the
         * trapped one -- walking it to the tail -- and then reset CVL from TVL
         * here, discarding the one value the guest was told to read.
         *
         * So the two cursors are separated. The LOCAL one always starts at the
         * head, because rendering must not stop -- that is the whole point of
         * SE_WHILE_TRAPPED. The GUEST-VISIBLE pair does not move while a trap
         * the guest has not serviced is outstanding. `hold` is that
         * distinction and is the only thing it does.
         *
         * NOT OFFERED AS THE TRAP STORM'S FIX, and it is not one: the storm is
         * measured in runs that predate both this and the local cursor. It is
         * the handshake the guest is entitled to, and it was intact before
         * today. */
        int hold = (d->regs[NV_PAPU_FECTL] & NV_PAPU_FECTL_FEMETHMODE)
                       == NV_PAPU_FECTL_FEMETHMODE_TRAPPED;
        if (!hold) CURSOR_SET(current, d->regs[top]);

        uint64_t walk_seen[MCPX_HW_MAX_VOICES / 64] = { 0 };
        int cycle_noted = 0;

        for (int i = 0; cur != 0xFFFF; i++) {
            if (i >= MCPX_HW_MAX_VOICES) {
                /* THIS BREAK HAS BEEN SILENT FOR THE LIFE OF THE FILE.
                 *
                 * It reported through DPRINTF, and DEBUG_MCPX is commented out
                 * at the top of this file (:26), so DPRINTF is
                 * `do { } while (0)`. A list that rings burns 256 iterations
                 * here every subframe, calls voice_process once per iteration,
                 * and said so nowhere -- which is the mechanical reason a ring
                 * that has been audible since audio first worked went unfound.
                 * The cycle detector below is the instrument that found it in
                 * the end; this is the older and blunter one, and it is worth
                 * keeping distinct because it also fires for a list that is
                 * merely LONGER than the hardware has voices, which is a
                 * different defect with a different cause.
                 *
                 * Bounded to the first eight, matching the out-of-range report
                 * below, because at 1500 subframes a second an unbounded line
                 * here is the log. g_apu_list_walk_cap is what survives the
                 * eighth: the count is printed on the [APU-WALKCAP] report line
                 * every reporting interval, so the number is readable for the
                 * whole run and not just its first moments. */
                static unsigned cap_reported;
                g_apu_list_walk_cap++;
                if (cap_reported < 8) {
                    cap_reported++;
                    fprintf(stderr,
                            "  [APU] voice list %d hit the %d-iteration walk "
                            "cap at handle 0x%04X (came from 0x%04X, head "
                            "0x%04X) -- the list has more entries than the "
                            "hardware has voices, i.e. it rings; stopping "
                            "this list\n",
                            list, MCPX_HW_MAX_VOICES, cur, came_from,
                            (unsigned)(uint16_t)d->regs[top]);
                    fflush(stderr);
                }
                break;
            }

            uint16_t v = cur;

            /* A VOICE SEEN TWICE IN ONE WALK IS A RING. See the [APU-CYCLE]
             * block for why that matters and why the break is opt-in. The
             * bitmap is 32 bytes on the stack, cleared once per list per
             * subframe; the test is two instructions per voice. */
            if (v < MCPX_HW_MAX_VOICES) {
                uint64_t bit = 1ULL << (v & 63);
                if (walk_seen[v >> 6] & bit) {
                    if (!cycle_noted) {
                        cycle_noted = 1;
                        g_apu_list_cycles_seen++;
                    }
                    g_apu_list_cycle_last = v;
                    g_apu_list_cycle_list = (unsigned)list;
                    g_apu_voice_in_cycle[v >> 6] |= bit;
                    if (mcpx_apu_cycle_break()) {
                        g_apu_list_cycle_breaks++;
                        break;
                    }
                } else {
                    walk_seen[v >> 6] |= bit;
                }
            }

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

            uint16_t nxt = (uint16_t)voice_get_mask(d, v,
                               NV_PAVS_VOICE_TAR_PITCH_LINK,
                               NV_PAVS_VOICE_TAR_PITCH_LINK_NEXT_VOICE_HANDLE);
            link_shadow_check(v, nxt, "walk");

            /* A voice whose next-handle is ITSELF is the end of the list.
             *
             * This is the driver's own convention, not an invention here.
             * CMcpxCore::SetupVoiceProcessor writes link(v) = v for all 256
             * voices at boot, and RemoveIdleVoice writes it again for every
             * voice it takes out: self-link means "not in any list". Our walk
             * did not honour it, so such a voice was a one-entry cycle -- the
             * walk raised its idle trap, returned with the cursor pinned on it,
             * and every voice behind it went unrendered for the rest of the
             * run. Measured at gameplay before this change: voice 3 raising
             * 43,070 traps against 243 retirements, with 70-81% of APU frames
             * trapped and the sound engine reaching 8% duty in steady state.
             *
             * The cycle forms when VOICE_ON prepends a voice that regs[top]
             * already names: link(selected) = regs[top] = selected. The insert
             * is xemu's and is correct -- hardware does the same, and the XDK
             * driver asserts on it -- so the fix belongs here, at the read,
             * where it also catches cycles created by guest stores this model
             * never sees (the voice register file lives in guest RAM).
             *
             * Terminating cannot lose a reachable voice: by the time the guest
             * re-ONs v it has already written link(v) = v itself, so whatever v
             * used to point at is unreachable through v regardless.
             *
             * RECOMP_APU_SELFLINK_END=0 restores the previous behaviour for
             * A/B, because a fix measured only against its own arm is not
             * measured. */
            /* Captured BEFORE the guard below rewrites nxt to 0xFFFF, which
             * is the only record that this voice named itself. */
            int self_linked = (nxt == v);
            if (self_linked && mcpx_apu_selflink_end()) {
                g_apu_selflink_terminated++;
                nxt = 0xFFFF;
            }
            if (!hold) CURSOR_SET(next, nxt);

            if (!voice_get_mask(d, v, NV_PAVS_VOICE_PAR_STATE,
                                NV_PAVS_VOICE_PAR_STATE_ACTIVE_VOICE)) {
                /* Before any suppression, coalescing or edge latch: every
                 * voice the hardware would have had something to say about,
                 * which is the denominator delivery is measured against.
                 *
                 * ANCHORED ON THE `if (!hold)` LINE ABOVE, because the bare
                 * ACTIVE_VOICE test appears twice in this file -- the other is
                 * inside voice_resample -- and the first version of this
                 * counter patched that one. It then reported delivered=18
                 * against found=6 and underflowed the difference to 1.8e19,
                 * which is the only reason the mistake was visible at all. */
                if (v < MCPX_HW_MAX_VOICES
                    && !(g_idle_seen_mask[v >> 6] & (1ULL << (v & 63)))) {
                    g_idle_seen_mask[v >> 6] |= 1ULL << (v & 63);
                    ++g_idle_seen_distinct;
                }
                /* Raise the trap once. Re-raising it for a second dead voice
                 * would overwrite the handle the guest has not read yet, and
                 * re-raising it for the SAME voice every frame is what produced
                 * 217 traps per retirement. */
                /* One outstanding trap is one outstanding trap.
                 *
                 * The front end is already TRAPPED and the guest has not
                 * serviced it yet, so raising again cannot mean anything to
                 * hardware: it would overwrite FEDECPARAM -- the handle the
                 * guest has not read -- and raise a second interrupt for a
                 * condition already signalled. The comment below already says
                 * this about a second dead voice inside one walk; the same
                 * argument holds across frames, which is the case it missed.
                 *
                 * It is load-bearing once the sound engine keeps running while
                 * trapped. Measured over 240 s of churn: with the engine
                 * switched off during a trap the storm self-throttles to about
                 * 10,300 raises, and with it running that becomes 126,181 --
                 * every one of them a guest interrupt into the DirectSound ISR
                 * that is already the top crash site here. Restoring the
                 * engine's frames must not be paid for in interrupts.
                 *
                 * RECOMP_APU_TRAP_COALESCE=0 disables, for A/B. */
                if (!trap_held && mcpx_apu_trap_coalesce() &&
                    (d->regs[NV_PAPU_FECTL] & NV_PAPU_FECTL_FEMETHMODE) ==
                        NV_PAPU_FECTL_FEMETHMODE_TRAPPED) {
                    g_apu_trap_suppressed++;
                    if (!mcpx_apu_se_while_trapped()) return;
                    trap_held = 1;
                    /* Redundant today -- reaching here with !trap_held and
                     * FEMETHMODE already TRAPPED means the trap was raised
                     * before this list's walk began, which is exactly what set
                     * `hold` above. Written out so that a later change to this
                     * condition cannot unfreeze the pair silently. */
                    hold = 1;
                }

                /* WHAT THIS VOICE WAS WHEN WE DECIDED TO REPORT IT.
                 *
                 * Gathered before the raise so it describes the state the
                 * decision was made on, and gathered whether or not the guard
                 * below is enabled -- the point is that a run with the guard
                 * OFF still says whether the guard would have mattered. Three
                 * guest-RAM reads and a bitmap test per raise, and raises are
                 * rare by construction (the coalescing above is what makes
                 * them rare): the whole cost lands on an event that already
                 * writes two MMIO registers and posts an interrupt. */
                int locked = is_voice_locked(d, v);
                uint32_t fmt = voice_get_mask(d, v, NV_PAVS_VOICE_CFG_FMT,
                                              0xFFFFFFFFu);
                int ever_on = (g_apu_voice_ever_on[v / 64]
                               & (1ULL << (v % 64))) != 0;

                /* HONOUR THE VOICE LOCK. See mcpx_apu_idle_trap_lock_guard for
                 * the window, the evidence, and why this is off by default.
                 *
                 * Counted whether or not it fires, so one run with the guard
                 * OFF answers "would it have fired on the raise that killed
                 * us" -- which is the only question that matters here and the
                 * one a guard that silently suppressed would destroy. */
                int suppress = 0;
                /* Counted whether or not it fires, so a run with the switch
                 * OFF still answers "how many raises would this have taken",
                 * which is the only number that decides whether to ship it. */
                if (self_linked) {
                    g_idle_trap_selflink_encounters++;
                    if (mcpx_apu_idle_trap_selflink()) {
                        if (!trap_held) g_idle_trap_selflink_suppressed++;
                        suppress = 1;
                    }
                }
                if (locked) {
                    g_idle_trap_locked_encounters++;
                    if (mcpx_apu_idle_trap_lock_guard()) {
                        /* Only when a raise would otherwise have happened.
                         * Counting it under trap_held too would have the guard
                         * claiming credit for raises the coalescing had already
                         * withheld. */
                        if (!trap_held) g_idle_trap_lock_suppressed++;
                        suppress = 1;
                    }
                }

                /* ONE RAISE PER IDLE TRANSITION. See mcpx_apu_idle_trap_edge
                 * for why the level reading cannot be hardware's.
                 *
                 * Gated on !trap_held && !suppress so that _encounters is the
                 * count of raises this rule would have withheld and nothing
                 * else -- a subset of g_idle_trap_raises, comparable with it,
                 * and not the different-denominator mistake the locked
                 * counters had to be split to undo. */
                if (v < MCPX_HW_MAX_VOICES && !trap_held && !suppress
                    && (g_idle_trap_reported[v >> 6] & (1ULL << (v & 63)))) {
                    g_idle_trap_edge_encounters++;
                    if (mcpx_apu_idle_trap_edge()) {
                        unsigned k = mcpx_apu_idle_trap_rearm_subframes();
                        if (k && ++g_idle_trap_edge_hold[v] >= k) {
                            /* Still inactive-and-linked one guest tick after
                             * we last said so: say it again. hold[] is reset
                             * where the latch is set, below. */
                            g_idle_trap_edge_reraise++;
                        } else {
                            g_idle_trap_edge_suppressed++;
                            suppress = 1;
                        }
                    }
                }

                /* Suppressing the RAISE only. The cursors below still advance
                 * exactly as they would for any voice with nothing to do this
                 * subframe -- a locked voice must not pin the walk, which is
                 * the failure mode the self-link guard above exists for. */
                if (!trap_held && !suppress) {
                    unsigned slot = (unsigned)(g_idle_trap_ring & 15u);
                    uint8_t why = 0;
                    if (locked)   why |= IDLE_TRAP_WHY_LOCKED;
                    if (!ever_on) why |= IDLE_TRAP_WHY_NEVER_ON;
                    if (fmt & NV_PAVS_VOICE_CFG_FMT_PERSIST)
                        why |= IDLE_TRAP_WHY_PERSIST;
                    if (g_idle_trap_ring
                        && g_idle_trap_last[(g_idle_trap_ring - 1) & 15u] == v)
                        why |= IDLE_TRAP_WHY_REPEAT;
                    /* WILL THE GUEST'S ISR DEREFERENCE A NULL FOR THIS?
                     *
                     * Read the guest's OWN table rather than a proxy for it.
                     * 001A200D does `pBuf = this->owner[h]` at
                     * this+0x2C4+h*4 with no NULL check.
                     *
                     * The voice-state proxy that stood here first was wrong
                     * and the measurement said so: it read 15,524 "unowned"
                     * raises on a run that did not crash once, because
                     * VOICE_OFF is a voice-state operation and owner[] is
                     * buffer lifetime. Those are different things and no
                     * combination of APU register traffic reconstructs the
                     * second.
                     *
                     * Only meaningful once the gen probe has captured `this`.
                     * Without it g_jsrf_dsound_this is 0, nothing is read, and
                     * the report says so rather than reporting a zero that
                     * looks like an answer -- which is the exact failure this
                     * counter has already had twice today. */
                    unsigned int dsound_this = jsrf_dsound_this();
                    if (dsound_this && v < 0x100) {
                        uint32_t owner = ldl_le_phys(
                            address_space_memory,
                            dsound_this + 0x2C4u + (uint32_t)v * 4u);
                        if (owner) ++g_idle_owner_ok_raises;
                        else {
                            ++g_idle_owner_null_raises;
                            why |= IDLE_TRAP_WHY_UNOWNED;
                            /* h==0 AND owner NULL is the only combination that
                             * reaches the faulting call. Counted separately so
                             * the guard's value is visible rather than
                             * inferred from the total. */
                            if (v == 0) ++g_idle_owner_null_h0;
                            if (owner_guard_on()) {
                                ++g_idle_owner_suppressed;
                                ++g_apu_trap_suppressed;
                                return;
                            }
                        }
                    }

                    if (locked)   g_idle_trap_locked_raises++;
                    if (!ever_on) g_idle_trap_never_on_raises++;
                    /* The total the ring's P flag has never had. A raise whose
                     * voice has PERSIST set is one the guest's ISR returns
                     * from without acting (001A241F), so it can never retire
                     * the voice and the level-triggered raise can only repeat.
                     * If this tracks raises, the storm is that loop. */
                    if (fmt & NV_PAVS_VOICE_CFG_FMT_PERSIST)
                        g_idle_trap_persist_raises++;
                    /* Latch: this voice has now been reported idle. Set
                     * whether or not the edge rule is enabled, so that the
                     * counters above measure it with the switch off. */
                    if (v < MCPX_HW_MAX_VOICES) {
                        g_idle_trap_reported[v >> 6] |= 1ULL << (v & 63);
                        g_idle_trap_edge_hold[v] = 0;
                    }
                    if (v < MCPX_HW_MAX_VOICES
                        && (g_apu_voice_in_cycle[v >> 6] & (1ULL << (v & 63))))
                        why |= IDLE_TRAP_WHY_CYCLE;
                    if (why & IDLE_TRAP_WHY_REPEAT) g_idle_trap_repeat_raises++;

                    voice_lifecycle_note(d, v, "idle");
                    g_idle_trap_raises++;
                    if (v < MCPX_HW_MAX_VOICES) g_idle_trap_by_voice[v]++;
                    g_idle_trap_last[slot] = (uint16_t)v;
                    g_idle_trap_why[slot]  = why;
                    g_idle_trap_from[slot] = came_from;
                    g_idle_trap_fmt[slot]  = fmt;
                    g_idle_trap_list[slot] = (uint8_t)list;
                    g_idle_trap_ring++;
                    /* The pair names the voice the guest is about to be told
                     * about. With coalescing off this raise REPLACES an
                     * outstanding one, so the cursor has to follow it to the
                     * new voice rather than stay frozen on the old -- otherwise
                     * FEDECPARAM and CVL would name different voices. With
                     * coalescing on this is the first raise and both are
                     * already correct; writing them is then a no-op. */
                    CURSOR_SET(current, v);
                    CURSOR_SET(next, nxt);
                    {
                        /* Delivered means the PAIR WAS WRITTEN, not that we
                         * raised. FEDEC_HOLD holds it at a trapped front end,
                         * and a held pair never reaches the guest's ISR. */
                        extern unsigned long g_apu_fedec_held;
                        unsigned long held_before = g_apu_fedec_held;
                        fe_method(d, SE2FE_IDLE_VOICE, v);
                        if (g_apu_fedec_held == held_before
                            && v < MCPX_HW_MAX_VOICES
                            && !(g_idle_delivered_mask[v >> 6]
                                 & (1ULL << (v & 63)))) {
                            g_idle_delivered_mask[v >> 6] |= 1ULL << (v & 63);
                            ++g_idle_delivered_distinct;
                        }
                    }
                    if ((d->regs[NV_PAPU_FECTL] & NV_PAPU_FECTL_FEMETHMODE) ==
                            NV_PAPU_FECTL_FEMETHMODE_TRAPPED) {
                        if (!mcpx_apu_se_while_trapped())
                            return;
                        /* Hold the pair here for the guest, and keep rendering
                         * everything behind it. */
                        trap_held = 1;
                        hold = 1;
                    }
                }
            } else {
                /* ACTIVE AGAIN, SO THE EDGE IS SPENT. Reaching here means the
                 * voice has ACTIVE_VOICE set, which is the only state from
                 * which it can transition to idle again. Clearing here as well
                 * as in VOICE_ON is what makes the latch self-healing: a voice
                 * restarted by any path this model does not see -- the voice
                 * register file lives in guest RAM -- still re-arms the first
                 * time the walk renders it. Two instructions on the hot path. */
                if (v < MCPX_HW_MAX_VOICES) {
                    uint64_t ebit = 1ULL << (v & 63);
                    if (g_idle_trap_reported[v >> 6] & ebit) {
                        g_idle_trap_reported[v >> 6] &= ~ebit;
                        g_idle_trap_edge_rearm++;
                    }
                }
                /* Process voice directly (single-threaded) */
                g_apu_voice_process_count++;
                voice_process(d, mixbins, d->vp.sample_buf, v, list);
            }
            if (!hold) CURSOR_SET(current, nxt);
            came_from = v;
            cur = nxt;
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
    /* Resolve the switch now, on an ordinary thread.
     *
     * It is a getenv behind a static, and the only other thing that forces it
     * is the first idle-trap raise. A run that crashes before ever raising one
     * would otherwise reach the first getenv from inside the SIGSEGV handler,
     * which prints this counter line -- and getenv is not async-signal-safe.
     * One call here removes the case entirely. */
    (void)mcpx_apu_idle_trap_lock_guard();
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
