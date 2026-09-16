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

/* Every handle the guest has ever issued VOICE_ON for, one bit each.
 *
 * Set in the VOICE_ON handler and never cleared: the question it answers is
 * "has DirectSound ever owned this voice", and a voice it has since retired
 * still has an owner slot. A handle that is still zero here when we hand it to
 * the ISR is one the guest has never seen, so owner[h] cannot be anything but
 * the NULL it was initialised to. */
uint64_t g_apu_voice_ever_on[MCPX_HW_MAX_VOICES / 64];

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
        on = e ? (atoi(e) != 0) : 1;
    }
    return on;
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

void mcpx_apu_voice_report(void)
{
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
    /* off_already: see voice_off. raises-vs-idle_trap: g_apu_idle_trap_count
     * only increments when FETFORCE1 has SE2FE_IDLE_VOICE armed, but fe_method
     * writes FEDECMETH/FEDECPARAM BEFORE that test -- so if the guest ever
     * clears FETFORCE1 the two diverge, and each unarmed raise still clobbers
     * the decode pair the guest's polling ISR reads. Equal in every run so far,
     * and nothing asserted it; printing the difference is what makes a
     * divergence visible the first time it happens. */
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
    fprintf(stderr, "  [APU-TRAP] suppressed=%lu (coalesce %s, se_while_trapped %s)\n",
            g_apu_trap_suppressed,
            mcpx_apu_trap_coalesce() ? "on" : "OFF",
            mcpx_apu_se_while_trapped() ? "on" : "OFF");
    fprintf(stderr, "  [APU-SELFLINK] terminated=%lu (guard %s)\n",
            g_apu_selflink_terminated,
            mcpx_apu_selflink_end() ? "on" : "OFF");
    fprintf(stderr, "  [APU-REON] head_nop=%lu (reon_head_nop %s)\n",
            g_apu_voice_on_head_nop,
            mcpx_apu_reon_head_nop() ? "on" : "OFF");
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
    fprintf(stderr, "  [APU-FEDEC] guest methods dispatched while TRAPPED:"
            " %lu (of %lu) -- each one overwrites the FEDECMETH/FEDECPARAM"
            " pair the guest has not read yet\n",
            g_apu_method_while_trapped, g_apu_guest_method_count);
    fprintf(stderr, "  [APU-FEDEC] decode pairs held while trapped: %lu"
            " (fedec_hold %s) -- a held pair is an ISR that read the handle it"
            " was sent, not the next method's argument\n",
            g_apu_fedec_held,
            getenv("RECOMP_APU_FEDEC_HOLD")
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
        char flags[6];
        unsigned f = 0;
        if (w & IDLE_TRAP_WHY_LOCKED)   flags[f++] = 'L';
        if (w & IDLE_TRAP_WHY_NEVER_ON) flags[f++] = 'N';
        if (w & IDLE_TRAP_WHY_PERSIST)  flags[f++] = 'P';
        if (w & IDLE_TRAP_WHY_REPEAT)   flags[f++] = 'R';
        if (w & IDLE_TRAP_WHY_CYCLE)    flags[f++] = 'C';
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
                    " early), R = repeat of the previous raise.\n"
                    "  2D/3D/MP is the voice list. fmt=0 is an unconfigured"
                    " voice. The LAST entry is the"
                    " handle the guest was servicing.\n");
}

static void voice_off(MCPXAPUState *d, uint16_t v)
{
    voice_lifecycle_note(d, v, "retire");
    g_apu_voice_off_count++;
    voice_ev_note(1, v);

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
    /* DO NOT CLOBBER AN UNREAD DECODE PAIR. RECOMP_APU_FEDEC_HOLD, default on.
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
         * shipping an unattributed change. */
        static int hold = -1;
        if (hold < 0) { const char *e = getenv("RECOMP_APU_FEDEC_HOLD");
                        hold = e ? (atoi(e) != 0) : 0; }
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
        if (selected_handle < MCPX_HW_MAX_VOICES)
            g_apu_voice_ever_on[selected_handle / 64] |=
                1ULL << (selected_handle % 64);
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

            if (d->regs[top_reg] == selected_handle
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
        voice_set_mask(d, (uint16_t)selected_handle, NV_PAVS_VOICE_PAR_STATE,
                       NV_PAVS_VOICE_PAR_STATE_ACTIVE_VOICE, 1);

        if (!locked) voice_lock(d, (uint16_t)selected_handle, false);
        break;
    }

    case NV1BA0_PIO_VOICE_RELEASE: {
        g_apu_voice_release_count++;
        selected_handle = argument & NV1BA0_PIO_VOICE_ON_HANDLE;
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
        if (!hold) d->regs[current] = d->regs[top];

        uint64_t walk_seen[MCPX_HW_MAX_VOICES / 64] = { 0 };
        int cycle_noted = 0;

        for (int i = 0; cur != 0xFFFF; i++) {
            if (i >= MCPX_HW_MAX_VOICES) {
                /* DPRINTF compiles to nothing, so this break has been silent
                 * for the life of the file -- a list that rings burns 256
                 * iterations here every subframe and says so nowhere. The
                 * cycle detector below is what makes it visible; this stays as
                 * the backstop for a list that is long rather than circular. */
                DPRINTF("Voice list contains invalid entry!\n");
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
            if (nxt == v && mcpx_apu_selflink_end()) {
                g_apu_selflink_terminated++;
                nxt = 0xFFFF;
            }
            if (!hold) d->regs[next] = nxt;

            if (!voice_get_mask(d, v, NV_PAVS_VOICE_PAR_STATE,
                                NV_PAVS_VOICE_PAR_STATE_ACTIVE_VOICE)) {
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

                    if (locked)   g_idle_trap_locked_raises++;
                    if (!ever_on) g_idle_trap_never_on_raises++;
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
                    d->regs[current] = v;
                    d->regs[next] = nxt;
                    fe_method(d, SE2FE_IDLE_VOICE, v);
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
                /* Process voice directly (single-threaded) */
                g_apu_voice_process_count++;
                voice_process(d, mixbins, d->vp.sample_buf, v, list);
            }
            if (!hold) d->regs[current] = nxt;
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
