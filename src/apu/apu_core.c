/*
 * MCPX APU Core - Standalone extraction from xemu
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
#include "apu.h"
#include "apu_sdl2.h"
#include "apu_xaudio2.h"
#include "fpconv.h"

/* ============================================================
 * Globals
 * ============================================================ */

uint8_t *g_apu_ram_ptr = NULL;

MCPXAPUState *g_state = NULL;

/* Forward declarations for software mixer */
static void mixer_init(void);
static void mixer_render(int16_t frame_buf[][2], int num_samples);
static APUMixerVoice g_mixer_voices[APU_MIXER_MAX_VOICES];
static volatile int g_mixer_active_count = 0;
static CRITICAL_SECTION g_mixer_cs;
static bool g_mixer_initialized = false;
struct McpxApuDebug g_dbg;
struct McpxApuDebug g_dbg_cache;
int g_dbg_voice_monitor = -1;
uint64_t g_dbg_muted_voices[4] = { 0 };

/* Global audio mute — disables all AWD/mixer sound playback */
volatile int g_audio_muted = 0;  /* 0 = audio enabled */

/* ============================================================
 * Debug frame markers (minimal stubs)
 * ============================================================ */

void mcpx_debug_begin_frame(void) {}
void mcpx_debug_end_frame(void) {}

/* ============================================================
 * IRQ handling (stubbed - no PCI bus in standalone)
 * ============================================================ */

static void update_irq(MCPXAPUState *d)
{
    /* FEMETHMODE is a field, not a flag, so it has to be masked before it is
     * compared. TRAPPED is 0xE0 and HALTED is 0x80, both inside the 0xE0 mask:
     * a bare AND against TRAPPED is therefore also true when the front end is
     * merely HALTED, and would raise the front-end trap interrupt for a mode
     * that has not trapped anything. The guest can reach that state -- FECTL
     * is writable from mcpx_apu_write -- so this is reachable, not academic. */
    if ((d->regs[NV_PAPU_FECTL] & NV_PAPU_FECTL_FEMETHMODE)
            == NV_PAPU_FECTL_FEMETHMODE_TRAPPED) {
        qatomic_or(&d->regs[NV_PAPU_ISTS], NV_PAPU_ISTS_FETINTSTS);
    }
    if ((d->regs[NV_PAPU_IEN] & NV_PAPU_ISTS_GINTSTS) &&
        ((d->regs[NV_PAPU_ISTS] & ~NV_PAPU_ISTS_GINTSTS) &
         d->regs[NV_PAPU_IEN])) {
        qatomic_or(&d->regs[NV_PAPU_ISTS], NV_PAPU_ISTS_GINTSTS);
        /* In standalone mode we don't raise a PCI IRQ; the game's kernel
         * stub will poll ISTS directly or we'll signal via a flag. */
        pci_irq_assert(PCI_DEVICE(d));
    } else {
        qatomic_and(&d->regs[NV_PAPU_ISTS], ~NV_PAPU_ISTS_GINTSTS);
        pci_irq_deassert(PCI_DEVICE(d));
    }
}

/* ============================================================
 * MMIO Read / Write
 * ============================================================ */

uint64_t mcpx_apu_read(void *opaque, hwaddr addr, unsigned int size)
{
    MCPXAPUState *d = (MCPXAPUState *)opaque;
    uint64_t r = 0;

    switch (addr) {
    case NV_PAPU_ISTS:
        update_irq(d);
        r = qatomic_read(&d->regs[addr]);
        break;
    case NV_PAPU_XGSCNT:
        r = (uint64_t)(qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) / 100);
        break;
    default:
        if (addr < 0x20000) {
            r = qatomic_read(&d->regs[addr]);
        }
        break;
    }

    /* Uncomment for register tracing:
     * fprintf(stderr, "[APU] read  [0x%05llX] size=%u -> 0x%08llX\n",
     *         (unsigned long long)addr, size, (unsigned long long)r);
     */
    (void)size;
    return r;
}

void mcpx_apu_write(void *opaque, hwaddr addr, uint64_t val,
                     unsigned int size)
{
    MCPXAPUState *d = (MCPXAPUState *)opaque;

    /* Uncomment for register tracing:
     * fprintf(stderr, "[APU] write [0x%05llX] size=%u <- 0x%08llX\n",
     *         (unsigned long long)addr, size, (unsigned long long)val);
     */
    (void)size;

    switch (addr) {
    case NV_PAPU_ISTS:
        qatomic_and(&d->regs[NV_PAPU_ISTS], ~(uint32_t)val);
        update_irq(d);
        qemu_cond_broadcast(&d->cond);
        break;
    case NV_PAPU_FECTL:
    case NV_PAPU_SECTL:
        qatomic_set(&d->regs[addr], (uint32_t)val);
        /* Starting the APU has to start the frame thread.
         *
         * The thread idles on pause_requested, which init sets and only the
         * test tone ever cleared -- so a title that enabled the APU through
         * these registers got an APU that stayed asleep. Nothing then advanced
         * the front end, and a title waiting on a notify completion (the
         * FEMEMDATA magic write, which is how completion reaches guest memory)
         * waited forever. Wreckless hangs exactly there during DirectSound
         * init, and because it initialises its whole engine behind a
         * successful DirectSound create, that hang is not confined to audio.
         *
         * Resume whenever the write is not switching the block off; the thread
         * re-checks FECTL itself and idles again if it is halted or trapped. */
        {
            uint32_t sectl = qatomic_read(&d->regs[NV_PAPU_SECTL]);
            uint32_t fectl = qatomic_read(&d->regs[NV_PAPU_FECTL]);
            bool running =
                ((sectl & NV_PAPU_SECTL_XCNTMODE) != NV_PAPU_SECTL_XCNTMODE_OFF)
                && ((fectl & NV_PAPU_FECTL_FEMETHMODE)
                    != NV_PAPU_FECTL_FEMETHMODE_HALTED);
            if (running && d->pause_requested) {
                d->pause_requested = false;
                fprintf(stderr, "[APU] started by the title"
                                " (SECTL=%08X FECTL=%08X)\n", sectl, fectl);
            }
        }
        qemu_cond_broadcast(&d->cond);
        break;
    case NV_PAPU_FEMEMDATA:
        /* 'magic write' - value written to FEMEMADDR on notify completion */
        stl_le_phys(address_space_memory, d->regs[NV_PAPU_FEMEMADDR], (uint32_t)val);
        qatomic_set(&d->regs[addr], (uint32_t)val);
        break;
    default:
        if (addr < 0x20000) {
            qatomic_set(&d->regs[addr], (uint32_t)val);
        }
        break;
    }
}

/* ============================================================
 * Test tone state (used by monitor and test tone functions)
 * ============================================================ */

static struct {
    bool active;
    double phase;
    double phase_inc;
    int16_t amplitude;
} g_test_tone = { false, 0.0, 0.0, 0 };

/* ============================================================
 * Monitor - Audio output (XAudio2 primary, waveOut fallback)
 * ============================================================ */

#if defined(_WIN32)
#include <mmsystem.h>
#pragma comment(lib, "winmm.lib")
#endif
/* On POSIX, SDL supplies the host output. waveOut remains the Windows
 * fallback. */

/* Ring of waveOut buffers for double-buffering */
#define WAVEOUT_NUM_BUFS 4
#define WAVEOUT_BUF_SAMPLES 256   /* Eight 32-sample APU subframes */
#define MIXER_FRAME_SAMPLES 256  /* Internal mixing frame size (matches frame_buf) */

typedef struct {
    HWAVEOUT hwo;
    WAVEHDR  hdrs[WAVEOUT_NUM_BUFS];
    int16_t  bufs[WAVEOUT_NUM_BUFS][WAVEOUT_BUF_SAMPLES][2];
    int      next_buf;
    bool     initialized;
    int      frames_written;
} WaveOutState;

static WaveOutState g_waveout = { 0 };

void mcpx_apu_monitor_init(MCPXAPUState *d, Error **errp)
{
    (void)errp;
    d->monitor.stream = NULL;
    d->monitor.queued_bytes_low = 1024;
    d->monitor.queued_bytes_high = 3072;

    /* Try XAudio2 first (lower latency) */
    if (xa2_init()) {
        fprintf(stderr, "[APU] Using XAudio2 audio backend\n");
        return;
    }
#if !defined(_WIN32)
    if (apu_sdl2_init()) {
        fprintf(stderr, "[APU] Using SDL2 audio backend\n");
        return;
    }
#endif
    fprintf(stderr, "[APU] XAudio2 unavailable, falling back to waveOut\n");

    WAVEFORMATEX wfx = { 0 };
    wfx.wFormatTag      = WAVE_FORMAT_PCM;
    wfx.nChannels       = 2;
    wfx.nSamplesPerSec  = 48000;
    wfx.wBitsPerSample  = 16;
    wfx.nBlockAlign     = wfx.nChannels * wfx.wBitsPerSample / 8;
    wfx.nAvgBytesPerSec = wfx.nSamplesPerSec * wfx.nBlockAlign;

    MMRESULT mr = waveOutOpen(&g_waveout.hwo, WAVE_MAPPER, &wfx,
                               0, 0, CALLBACK_NULL);
    if (mr != MMSYSERR_NOERROR) {
        fprintf(stderr, "[APU] waveOutOpen failed (error %u)\n", mr);
        g_waveout.initialized = false;
        return;
    }

    /* Prepare all headers */
    for (int i = 0; i < WAVEOUT_NUM_BUFS; i++) {
        memset(&g_waveout.hdrs[i], 0, sizeof(WAVEHDR));
        g_waveout.hdrs[i].lpData = (LPSTR)g_waveout.bufs[i];
        g_waveout.hdrs[i].dwBufferLength = WAVEOUT_BUF_SAMPLES * 2 * sizeof(int16_t);
        waveOutPrepareHeader(g_waveout.hwo, &g_waveout.hdrs[i], sizeof(WAVEHDR));
    }

    g_waveout.next_buf = 0;
    g_waveout.initialized = true;
    g_waveout.frames_written = 0;

    fprintf(stderr, "[APU] waveOut audio output initialized (48kHz stereo 16-bit, %d buffers)\n",
            WAVEOUT_NUM_BUFS);
}

void mcpx_apu_monitor_finalize(MCPXAPUState *d)
{
    (void)d;
    if (xa2_is_active()) {
        xa2_shutdown();
        return;
    }
    if (apu_sdl2_is_active()) {
        apu_sdl2_shutdown();
        return;
    }
    if (!g_waveout.initialized) return;

    waveOutReset(g_waveout.hwo);
    for (int i = 0; i < WAVEOUT_NUM_BUFS; i++) {
        waveOutUnprepareHeader(g_waveout.hwo, &g_waveout.hdrs[i], sizeof(WAVEHDR));
    }
    waveOutClose(g_waveout.hwo);
    g_waveout.initialized = false;
    fprintf(stderr, "[APU] waveOut audio output shut down (%d frames written)\n",
            g_waveout.frames_written);
}

void mcpx_apu_monitor_frame(MCPXAPUState *d)
{
    int output_samples = 8 * NUM_SAMPLES_PER_FRAME;

    if ((d->ep_frame_div + 1) % 8) {
        return;
    }

    /* VP/DSP has filled this buffer across eight 32-sample subframes. The old
     * output paths cleared it here and therefore submitted only silence (or
     * the separate software mixer). Add diagnostics/bridge voices in place. */
    if (g_test_tone.active && !g_audio_muted) {
        for (int i = 0; i < output_samples; i++) {
            int16_t s = (int16_t)(sin(g_test_tone.phase) * g_test_tone.amplitude);
            d->monitor.frame_buf[i][0] = s;
            d->monitor.frame_buf[i][1] = s;
            g_test_tone.phase += g_test_tone.phase_inc;
            if (g_test_tone.phase >= 2.0 * M_PI)
                g_test_tone.phase -= 2.0 * M_PI;
        }
    }
    if (!g_audio_muted)
        mixer_render(d->monitor.frame_buf, output_samples);
    else
        memset(d->monitor.frame_buf, 0, sizeof(d->monitor.frame_buf));

    if (xa2_is_active())
        xa2_submit_samples((const int16_t *)d->monitor.frame_buf, output_samples);
    else if (apu_sdl2_is_active())
        apu_sdl2_submit_samples((const int16_t *)d->monitor.frame_buf, output_samples);
    else if (g_waveout.initialized) {
        int idx = g_waveout.next_buf;
        WAVEHDR *hdr = &g_waveout.hdrs[idx];

        /* Wait if this buffer is still playing (with timeout). */
        int wait_loops = 0;
        while (!(hdr->dwFlags & WHDR_DONE) && (hdr->dwFlags & WHDR_INQUEUE)) {
            qemu_mutex_unlock(&d->lock);
            Sleep(1);
            qemu_mutex_lock(&d->lock);
            if (++wait_loops > 50) break;
        }
        memcpy(g_waveout.bufs[idx], d->monitor.frame_buf,
               output_samples * 2 * sizeof(int16_t));
        hdr->dwFlags &= ~WHDR_DONE;
        waveOutWrite(g_waveout.hwo, hdr, sizeof(WAVEHDR));
        g_waveout.next_buf = (idx + 1) % WAVEOUT_NUM_BUFS;
        g_waveout.frames_written++;
    }

    memset(d->monitor.frame_buf, 0, sizeof(d->monitor.frame_buf));
}

/* ============================================================
 * Throttle (timing control for frame pacing)
 * ============================================================ */

/* Pacing accounting.
 *
 * The frame thread is supposed to run at one EP frame per EP_FRAME_US, which
 * is 256 samples per 5333 us -- exactly 48 kHz. Whether it actually does is
 * not observable from anywhere else: se_frame's own frame_count is reset every
 * second for a utilisation figure that nothing prints. These are monotonic, so
 * the effective generated rate over a whole run can be divided out. */
unsigned long g_apu_subframes;          /* ep_frame_div increments */
unsigned long g_apu_se_frames;          /* full VP/DSP pipeline runs */
unsigned long g_apu_light_frames;       /* monitor-only runs */
unsigned long g_apu_throttle_calls;     /* throttle() reached the wait */
unsigned long g_apu_throttle_unpaced;   /* ... and returned without waiting */
unsigned long long g_apu_throttle_slept_us;
static int64_t g_apu_pace_start_us;

void mcpx_apu_pacing_report(void)
{
    extern unsigned long g_apu_sdl_batches, g_apu_sdl_frames, g_apu_sdl_clears;
    extern unsigned long g_apu_sdl_starved, g_apu_sdl_empty, g_apu_sdl_min_bytes;
    extern unsigned long g_apu_sdl_max_bytes, g_apu_sdl_depth_hist[6];
    extern unsigned long g_apu_sdl_prime_bytes, g_apu_sdl_reprimes;
    extern unsigned long g_apu_sdl_max_gap_us, g_apu_sdl_gaps_over_cushion;
    extern unsigned long apu_sdl2_queued_bytes(void);
    int64_t now_us = qemu_clock_get_us(QEMU_CLOCK_REALTIME);
    double elapsed_s = g_apu_pace_start_us
        ? (now_us - g_apu_pace_start_us) / 1000000.0 : 0.0;
    double gen_hz = elapsed_s > 0.0 ? g_apu_sdl_frames / elapsed_s : 0.0;

    fprintf(stderr, "  [APU-PACE] elapsed=%.1fs subframes=%lu se=%lu light=%lu"
            " throttle=%lu unpaced=%lu slept=%.1fs\n",
            elapsed_s, g_apu_subframes, g_apu_se_frames, g_apu_light_frames,
            g_apu_throttle_calls, g_apu_throttle_unpaced,
            g_apu_throttle_slept_us / 1000000.0);
    fprintf(stderr, "  [APU-PACE] batches=%lu frames=%lu gen_hz=%.0f"
            " queued=%lu bytes (%lu frames) clears=%lu\n",
            g_apu_sdl_batches, g_apu_sdl_frames, gen_hz,
            apu_sdl2_queued_bytes(), apu_sdl2_queued_bytes() / 4,
            g_apu_sdl_clears);
    /* The starvation end. min_bytes is the headroom that was actually left at
     * the worst moment of the run; starved counts submits that found less than
     * one device buffer still queued, which is the state a single late frame
     * turns into silence. */
    fprintf(stderr, "  [APU-PACE] starved=%lu empty=%lu min_queued=%lu bytes"
            " (%.1f ms)\n",
            g_apu_sdl_starved, g_apu_sdl_empty,
            g_apu_sdl_min_bytes == (unsigned long)-1 ? 0UL : g_apu_sdl_min_bytes,
            (g_apu_sdl_min_bytes == (unsigned long)-1 ? 0.0
                : g_apu_sdl_min_bytes / 4.0 / 48.0));
    /* Startup priming and steady-state refill are separate questions, so they
     * are reported separately: prime_bytes/reprimes describe getting the
     * cushion in place, the histogram describes whether it stays there. */
    fprintf(stderr, "  [APU-SDL2] prime=%lu bytes reprimes=%lu max=%lu bytes"
            " (%.1f ms)\n",
            g_apu_sdl_prime_bytes, g_apu_sdl_reprimes, g_apu_sdl_max_bytes,
            g_apu_sdl_max_bytes / 192.0);
    fprintf(stderr, "  [APU-SDL2] depth buckets (device buffers):"
            " 0=%lu <1=%lu 1-2=%lu 2-4=%lu 4-8=%lu 8+=%lu\n",
            g_apu_sdl_depth_hist[0], g_apu_sdl_depth_hist[1],
            g_apu_sdl_depth_hist[2], g_apu_sdl_depth_hist[3],
            g_apu_sdl_depth_hist[4], g_apu_sdl_depth_hist[5]);
    fprintf(stderr, "  [APU-SDL2] max_submit_gap=%.1f ms gaps_over_cushion=%lu\n",
            g_apu_sdl_max_gap_us / 1000.0, g_apu_sdl_gaps_over_cushion);
    fflush(stderr);
}

static void throttle(MCPXAPUState *d)
{
    if (d->ep_frame_div % 8) {
        return;
    }
    g_apu_throttle_calls++;
    if (!g_apu_pace_start_us)
        g_apu_pace_start_us = qemu_clock_get_us(QEMU_CLOCK_REALTIME);
    if (d->pause_requested)
        g_apu_throttle_unpaced++;

    int64_t now_us = qemu_clock_get_us(QEMU_CLOCK_REALTIME);

    if (d->next_frame_time_us == 0 ||
        now_us - d->next_frame_time_us > EP_FRAME_US) {
        d->next_frame_time_us = now_us;
    }

    int64_t wait_start_us = now_us;
    while (!d->pause_requested) {
        now_us = qemu_clock_get_us(QEMU_CLOCK_REALTIME);
        int64_t remaining_ms = (d->next_frame_time_us - now_us) / 1000;
        if (remaining_ms > 0) {
            qemu_cond_timedwait(&d->cond, &d->lock, (int)remaining_ms);
        } else {
            break;
        }
    }
    d->next_frame_time_us += EP_FRAME_US;

    /* Measured from before the wait loop. now_us is whatever the loop's last
     * iteration read -- the moment it decided not to wait any longer -- so
     * differencing against it reports approximately zero however long the
     * throttle actually blocked. sleep_acc_us, which feeds the utilisation
     * figure, has had that bug all along; it is preserved here rather than
     * changed, because nothing prints it. */
    {
        int64_t end_us = qemu_clock_get_us(QEMU_CLOCK_REALTIME);
        d->sleep_acc_us += (int)(end_us - now_us);
        if (end_us > wait_start_us)
            g_apu_throttle_slept_us +=
                (unsigned long long)(end_us - wait_start_us);
    }
}

/* ============================================================
 * se_frame - Process one audio frame (VP -> GP -> EP pipeline)
 * ============================================================ */

static void se_frame(MCPXAPUState *d)
{
    mcpx_apu_update_dsp_preference(d);
    mcpx_debug_begin_frame();
    g_dbg.gp_realtime = d->gp.realtime;
    g_dbg.ep_realtime = d->ep.realtime;

    int64_t now_ms = qemu_clock_get_ms(QEMU_CLOCK_REALTIME);
    int64_t elapsed_ms = now_ms - d->frame_count_time_ms;
    if (elapsed_ms >= 1000) {
        g_dbg.utilization = 1.0f - d->sleep_acc_us / (elapsed_ms * 1000.0f);
        g_dbg.frames_processed = (int)(d->frame_count * 1000.0 / elapsed_ms + 0.5);
        d->frame_count_time_ms = now_ms;
        d->frame_count = 0;
        d->sleep_acc_us = 0;
    }
    d->frame_count++;

    /* Buffer for all mixbins for this frame */
    float mixbins[NUM_MIXBINS][NUM_SAMPLES_PER_FRAME];
    memset(mixbins, 0, sizeof(mixbins));

    mcpx_apu_vp_frame(d, mixbins);
    mcpx_apu_dsp_frame(d, mixbins);
    mcpx_apu_monitor_frame(d);

    d->ep_frame_div++;

    mcpx_debug_end_frame();
}

/* Frame accounting for the sound-engine gate.
 *
 * se_frame is skipped whenever the front end is not free-running, and that
 * includes TRAPPED -- the mode every voice retirement drives. Whether the skip
 * matters is not answerable from a rate: one frame is 32 samples, 0.67 ms at
 * 48 kHz, so losing scattered single frames is inaudible while losing a run of
 * them is a dropout. So count the reasons separately and keep the longest
 * unbroken trapped run, which is the number the question actually turns on.
 *
 * Counters only; nothing here changes what the thread does. */
unsigned long g_apu_frames_total;
unsigned long g_apu_frames_se;         /* se_frame ran */
unsigned long g_apu_frames_trapped;    /* skipped: front end trapped */
unsigned long g_apu_frames_halted;     /* skipped: front end halted */
unsigned long g_apu_frames_xcnt_off;   /* skipped: sample counter off */
unsigned long g_apu_frames_tone;       /* skipped: test tone owns the output */
static unsigned long g_apu_trapped_run;
unsigned long g_apu_trapped_run_max;
/* Incremented once per frame boundary at which a guest thread was found
 * waiting for the device lock, immediately before the lock is dropped for it.
 * Zero means no guest thread ever waited -- not that the hand-off is dead. */
unsigned long g_apu_lock_handoffs;

void mcpx_apu_frame_report(void)
{
    double ms = g_apu_trapped_run_max * (double)NUM_SAMPLES_PER_FRAME / 48.0;
    fprintf(stderr, "  [APU-FRAME] total=%lu se=%lu trapped=%lu halted=%lu"
            " xcnt_off=%lu tone=%lu longest_trapped_run=%lu (%.2f ms)"
            " lock_handoffs=%lu\n",
            g_apu_frames_total, g_apu_frames_se, g_apu_frames_trapped,
            g_apu_frames_halted, g_apu_frames_xcnt_off, g_apu_frames_tone,
            g_apu_trapped_run_max, ms, g_apu_lock_handoffs);
    fflush(stderr);
}

/* ============================================================
 * Device lock hand-off
 * ============================================================ */

/*
 * The frame thread takes d->lock once, before its loop, and gives it up only
 * inside a cond wait -- which it reaches only from throttle(), and only when it
 * is running ahead of real time. While the mixer is saturated (JSRF's ADPCM
 * decode will do it on its own) `remaining_ms` is never positive, the wait is
 * never entered, and the lock is never released at all.
 *
 * That is enough to freeze the whole title, measured on a live process:
 *
 *   main thread  blocked in the guest's own DSOUND critical section,
 *                owned by a guest sound thread
 *   that thread  blocked on d->lock inside a TRAPPED APU register write
 *                (mcpx_trap_handler -> fe_method -> voice_lock)
 *   d->lock      owned by the frame thread, 100% busy in voice_process
 *
 * The guest's exec phase runs inside that DSOUND lock, so every object stops
 * updating while audio keeps playing and the USB model keeps polling -- it
 * reads as "the game hung" with no fault and no stopped counter anywhere in
 * the guest.
 *
 * Waiters announce themselves through mcpx_apu_lock_guest, and the frame
 * thread stands aside for them at its frame boundary, where model state is
 * consistent. The yield loop is bounded: a lost decrement must not turn this
 * into the hang it exists to prevent.
 */
#if defined(_WIN32)
#define APU_LOCK_YIELD() SwitchToThread()
#else
#include <sched.h>
#define APU_LOCK_YIELD() sched_yield()
#endif

#define APU_LOCK_HANDOFF_YIELDS 256

void mcpx_apu_lock_guest(MCPXAPUState *d)
{
    qatomic_fetch_add(&d->lock_waiters, 1);
    qemu_mutex_lock(&d->lock);
    qatomic_fetch_add(&d->lock_waiters, -1);
}

void mcpx_apu_unlock_guest(MCPXAPUState *d)
{
    qemu_mutex_unlock(&d->lock);
}

/* Called by the frame thread with the lock held. */
static void apu_lock_handoff(MCPXAPUState *d)
{
    volatile int *waiters = (volatile int *)&d->lock_waiters;
    int spins;

    if (!*waiters)
        return;
    g_apu_lock_handoffs++;
    qemu_mutex_unlock(&d->lock);
    for (spins = 0; spins < APU_LOCK_HANDOFF_YIELDS && *waiters; spins++)
        APU_LOCK_YIELD();
    qemu_mutex_lock(&d->lock);
}

/* ============================================================
 * APU frame thread (background processing)
 * ============================================================ */

static void *mcpx_apu_frame_thread(void *arg)
{
    MCPXAPUState *d = MCPX_APU_DEVICE(arg);
    qemu_mutex_lock(&d->lock);

    while (!qatomic_read(&d->exiting)) {
        if (d->pause_requested && !g_test_tone.active && !g_mixer_active_count) {
            d->is_idle = true;
            qemu_cond_signal(&d->idle_cond);
            qemu_cond_wait(&d->cond, &d->lock);
            d->is_idle = false;
            continue;
        }

        /* Always run the audio output loop — the software mixer and test tone
         * need continuous frame delivery regardless of APU register state.
         * The VP/DSP pipeline (se_frame) only runs when registers allow it. */
        throttle(d);

        if (d->set_irq) {
            update_irq(d);
            d->set_irq = false;
        }

        int xcntmode = GET_MASK(qatomic_read(&d->regs[NV_PAPU_SECTL]),
                                NV_PAPU_SECTL_XCNTMODE);
        uint32_t fectl = qatomic_read(&d->regs[NV_PAPU_FECTL]);
        /* Same field, same rule as update_irq. This one is latent rather than
         * wrong today: for the three defined mode values the bare ANDs happen
         * to agree with the intent, because HALTED's bit is inside TRAPPED's
         * mask. Write it as the comparison it means so it keeps agreeing.
         *
         * Idling the frame on TRAPPED is deliberate -- see mcpx_apu_write --
         * and is left alone here; whether the sound engine should keep running
         * while the front end is trapped is a separate question, and one that
         * needs a measurement rather than an edit. */
        uint32_t femethmode = fectl & NV_PAPU_FECTL_FEMETHMODE;
        bool apu_active = (xcntmode != NV_PAPU_SECTL_XCNTMODE_OFF) &&
                          femethmode != NV_PAPU_FECTL_FEMETHMODE_TRAPPED &&
                          femethmode != NV_PAPU_FECTL_FEMETHMODE_HALTED;

        g_apu_frames_total++;
        if (!apu_active) {
            if (xcntmode == NV_PAPU_SECTL_XCNTMODE_OFF)
                g_apu_frames_xcnt_off++;
            else if (femethmode == NV_PAPU_FECTL_FEMETHMODE_TRAPPED)
                g_apu_frames_trapped++;
            else if (femethmode == NV_PAPU_FECTL_FEMETHMODE_HALTED)
                g_apu_frames_halted++;
        } else if (g_test_tone.active) {
            g_apu_frames_tone++;
        }
        if (femethmode == NV_PAPU_FECTL_FEMETHMODE_TRAPPED) {
            if (++g_apu_trapped_run > g_apu_trapped_run_max)
                g_apu_trapped_run_max = g_apu_trapped_run;
        } else {
            g_apu_trapped_run = 0;
        }

        if (apu_active && !g_test_tone.active) {
            /* Full pipeline: VP voices → DSP → monitor → waveOut */
            g_apu_frames_se++;
            g_apu_se_frames++;
            g_apu_subframes++;
            se_frame(d);
        } else {
            /* Lightweight: just monitor frame (test tone + software mixer) */
            g_apu_light_frames++;
            g_apu_subframes++;
            mcpx_apu_monitor_frame(d);
            d->ep_frame_div++;
        }

        /* Frame boundary: the model is consistent here, so this is where a
         * waiting guest thread can be let in. */
        apu_lock_handoff(d);
    }

    qemu_mutex_unlock(&d->lock);
    return NULL;
}

/* ============================================================
 * Wait for idle / resume helpers
 * ============================================================ */

static void mcpx_apu_wait_for_idle(MCPXAPUState *d)
{
    d->pause_requested = true;
    qemu_cond_signal(&d->cond);
    while (!d->is_idle) {
        qemu_cond_wait(&d->idle_cond, &d->lock);
    }
}

static void mcpx_apu_resume(MCPXAPUState *d)
{
    d->pause_requested = false;
    qemu_cond_signal(&d->cond);
}

/* ============================================================
 * Reset
 * ============================================================ */

static void mcpx_apu_reset_locked(MCPXAPUState *d)
{
    memset(d->regs, 0, sizeof(d->regs));
    mcpx_apu_vp_reset(d);

    if (d->gp.dsp) {
        memset((void *)d->gp.dsp->core.pram_opcache, 0,
               sizeof(d->gp.dsp->core.pram_opcache));
    }
    if (d->ep.dsp) {
        memset((void *)d->ep.dsp->core.pram_opcache, 0,
               sizeof(d->ep.dsp->core.pram_opcache));
    }
    d->set_irq = false;
}

/* ============================================================
 * Public API: Init / Shutdown
 * ============================================================ */

MCPXAPUState *mcpx_apu_init_standalone(uint8_t *ram_ptr)
{
    MCPXAPUState *d = (MCPXAPUState *)calloc(1, sizeof(MCPXAPUState));
    if (!d) {
        fprintf(stderr, "[APU] Failed to allocate MCPXAPUState\n");
        return NULL;
    }

    g_apu_ram_ptr = ram_ptr;
    g_state = d;
    d->ram_ptr = ram_ptr;

    d->set_irq = false;
    d->exiting = false;
    d->is_idle = false;
    d->pause_requested = true;

    qemu_mutex_init(&d->lock);
    qemu_mutex_lock(&d->lock);
    qemu_cond_init(&d->cond);
    qemu_cond_init(&d->idle_cond);

    /* Init VP (voice processor) */
    mcpx_apu_vp_init(d);

    /* Init DSP (GP/EP - stubbed) */
    mcpx_apu_dsp_init(d);

    /* Init software mixer for DirectSound bridge */
    mixer_init();

    /* Init monitor (waveOut output) */
    Error *local_err = NULL;
    mcpx_apu_monitor_init(d, &local_err);
    if (local_err) {
        warn_reportf_err(local_err, "mcpx_apu_monitor_init failed: ");
    }

    /* Start background frame thread */
    qemu_thread_create(&d->apu_thread, "mcpx.apu_thread",
                       mcpx_apu_frame_thread, d, QEMU_THREAD_JOINABLE);
    mcpx_apu_wait_for_idle(d);
    qemu_mutex_unlock(&d->lock);

    fprintf(stderr, "[APU] MCPX APU initialized (standalone)\n");
    fprintf(stderr, "[APU]   RAM pointer: %p\n", (void *)ram_ptr);
    fprintf(stderr, "[APU]   MMIO base: 0xFE800000 (512KB)\n");
    fprintf(stderr, "[APU]   VP: %d max voices, %d samples/frame\n",
            MCPX_HW_MAX_VOICES, NUM_SAMPLES_PER_FRAME);
    return d;
}

void mcpx_apu_shutdown(MCPXAPUState *d)
{
    if (!d) return;

    fprintf(stderr, "[APU] Shutting down MCPX APU...\n");

    qemu_mutex_lock(&d->lock);
    mcpx_apu_wait_for_idle(d);
    qatomic_set(&d->exiting, true);
    qemu_cond_signal(&d->cond);
    qemu_mutex_unlock(&d->lock);

    qemu_thread_join(&d->apu_thread);
    mcpx_apu_vp_finalize(d);
    mcpx_apu_monitor_finalize(d);

    free(d);
    g_state = NULL;
    fprintf(stderr, "[APU] Shutdown complete\n");
}

/* ============================================================
 * VP MMIO handlers (sub-region at +0x20000)
 *
 * These are called when the game writes to the VP PIO registers
 * to configure voices, SSL, etc.
 * ============================================================ */

uint64_t mcpx_apu_vp_read(void *opaque, hwaddr addr, unsigned int size);
void mcpx_apu_vp_write(void *opaque, hwaddr addr, uint64_t val, unsigned int size);

/* Dispatch a VP-region access (offset 0x20000-0x2FFFF from APU base) */
void mcpx_apu_dispatch_mmio(MCPXAPUState *d, hwaddr addr, uint64_t val,
                             unsigned int size, bool is_write)
{
    if (addr >= 0x20000 && addr < 0x30000) {
        /* VP region */
        hwaddr vp_addr = addr - 0x20000;
        if (is_write) {
            mcpx_apu_vp_write(d, vp_addr, val, size);
        }
        /* VP reads handled by caller if needed */
    } else if (addr < 0x20000) {
        /* Main APU registers */
        if (is_write) {
            mcpx_apu_write(d, addr, val, size);
        }
    } else if (addr >= 0x30000 && addr < 0x40000) {
        /* GP, and EP below, are stubbed DSPs -- but "stubbed" has to mean the
         * register file still behaves like storage, because the guest does
         * READ-MODIFY-WRITE on it. JSRF brings the EP out of reset with
         * EPRST |= 1 at 0x5FFFC; dropping the write and reading back 0 turns
         * that into a write of 0, the DSP stays in reset, and the title never
         * enables the sound engine -- no SECTL, so no APU frames, no ADX, no
         * loading. Measured on Windows, where PAGE_NOACCESS traps reads and
         * they reach this function. macOS never saw it: there only writes
         * trap, reads are ordinary loads against the aperture RAM that the
         * trap handler stores through, so the guest reads its own last write
         * back and the sequence works by accident. */
        if (is_write)
            d->gp.regs[addr - 0x30000] = (uint32_t)val;
    } else if (addr >= 0x50000 && addr < 0x60000) {
        if (is_write)
            d->ep.regs[addr - 0x50000] = (uint32_t)val;
    }
}

/* ============================================================
 * Public MMIO API (called from VEH or MMIO hook)
 * addr is offset from APU base (0xFE800000)
 * ============================================================ */

uint64_t mcpx_apu_mmio_read(MCPXAPUState *d, uint64_t addr, unsigned int size)
{
    if (!d) return 0;
    if (addr >= 0x20000 && addr < 0x30000) {
        return mcpx_apu_vp_read(d, addr - 0x20000, size);
    } else if (addr < 0x20000) {
        return mcpx_apu_read(d, (hwaddr)addr, size);
    } else if (addr >= 0x30000 && addr < 0x40000) {
        return d->gp.regs[addr - 0x30000];
    } else if (addr >= 0x50000 && addr < 0x60000) {
        return d->ep.regs[addr - 0x50000];
    }
    return 0;
}

/* Which guest function is doing the store, supplied by the harness because the
 * model has no way to know. Optional: unset, the trace still prints the
 * register traffic. */
static uint32_t (*g_apu_trace_pc_fn)(void);

void mcpx_apu_set_trace_pc_fn(uint32_t (*fn)(void))
{
    g_apu_trace_pc_fn = fn;
}

void mcpx_apu_mmio_write(MCPXAPUState *d, uint64_t addr, uint64_t val, unsigned int size)
{
    /* RECOMP_APU_REG_TRACE -- every APU register write, on BOTH hosts, with
     * the guest function that issued it.
     *
     * This is the one funnel both paths reach: on Windows the VEH decodes the
     * faulting store and calls here, on macOS the signal handler calls here
     * through xbox_SetApuMmioWriteHook. Both run on the faulting guest thread,
     * so the harness's thread-local "current guest function" is the right one.
     *
     * The point is cross-host comparison: the same register written with a
     * different value names the guest function to look at, which register
     * offsets alone never do. */
    static int on = -1;
    static unsigned n;
    if (on < 0) on = getenv("RECOMP_APU_REG_TRACE") ? 1 : 0;
    if (on && n++ < 600) {
        uint32_t pc = g_apu_trace_pc_fn ? g_apu_trace_pc_fn() : 0;
        fprintf(stderr, "  [APUREG] 0x%05X = %08X (w%u) from sub_%08X\n",
                (unsigned)addr, (uint32_t)val, size, pc);
        fflush(stderr);
    }
    if (!d) return;
    mcpx_apu_dispatch_mmio(d, (hwaddr)addr, val, size, true);
}

/* ============================================================
 * APU Test Tone - Direct waveOut sine generator
 *
 * Bypasses the VP pipeline entirely and writes a 440Hz sine wave
 * directly to the monitor frame_buf. This verifies that waveOut
 * output works correctly.
 * ============================================================ */

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

void mcpx_apu_play_test_tone(MCPXAPUState *d)
{
    if (!d) {
        fprintf(stderr, "[APU-TEST] No APU state\n");
        return;
    }

    if (g_test_tone.active) {
        /* Toggle off */
        g_test_tone.active = false;
        fprintf(stderr, "[APU-TEST] Test tone OFF\n");
        return;
    }

    /* 440Hz at 48kHz sample rate */
    g_test_tone.phase = 0.0;
    g_test_tone.phase_inc = 2.0 * M_PI * 440.0 / 48000.0;
    g_test_tone.amplitude = 6000;  /* ~18% of full scale */
    g_test_tone.active = true;

    /* Make sure waveOut is running - enable SECTL and resume APU thread */
    qemu_mutex_lock(&d->lock);
    d->regs[NV_PAPU_SECTL] = NV_PAPU_SECTL_XCNTMODE & ~NV_PAPU_SECTL_XCNTMODE_OFF;
    d->regs[NV_PAPU_FECTL] = NV_PAPU_FECTL_FEMETHMODE_FREE_RUNNING;
    /* Initialize empty voice lists so VP frame doesn't crash */
    d->regs[NV_PAPU_TVL2D] = 0xFFFF;
    d->regs[NV_PAPU_TVL3D] = 0xFFFF;
    d->regs[NV_PAPU_TVLMP] = 0xFFFF;
    mcpx_apu_resume(d);
    qemu_mutex_unlock(&d->lock);

    fprintf(stderr, "[APU-TEST] Test tone ON - 440Hz sine, amplitude=%d\n",
            g_test_tone.amplitude);
}

/* ============================================================
 * Software mixer - mixes DirectSound buffers to waveOut
 *
 * This bypasses the VP hardware voice pipeline entirely.
 * DirectSound buffers register PCM data here, and the APU
 * frame thread mixes them into the monitor frame_buf.
 * ============================================================ */

static void mixer_init(void)
{
    if (g_mixer_initialized) return;
    InitializeCriticalSection(&g_mixer_cs);
    memset(g_mixer_voices, 0, sizeof(g_mixer_voices));
    g_mixer_initialized = true;
}

int apu_mixer_alloc_voice(void)
{
    if (!g_mixer_initialized) mixer_init();
    EnterCriticalSection(&g_mixer_cs);
    for (int i = 0; i < APU_MIXER_MAX_VOICES; i++) {
        if (!g_mixer_voices[i].active && !g_mixer_voices[i].pcm_data) {
            g_mixer_voices[i].volume = 1.0f;
            g_mixer_voices[i].sample_rate = 44100;
            g_mixer_voices[i].num_channels = 2;
            LeaveCriticalSection(&g_mixer_cs);
            return i;
        }
    }
    LeaveCriticalSection(&g_mixer_cs);
    return -1;
}

void apu_mixer_free_voice(int slot)
{
    if (slot < 0 || slot >= APU_MIXER_MAX_VOICES) return;
    EnterCriticalSection(&g_mixer_cs);
    apu_mixer_stop(slot);
    g_mixer_voices[slot].pcm_data = NULL;
    g_mixer_voices[slot].pcm_bytes = 0;
    g_mixer_voices[slot].play_offset = 0;
    LeaveCriticalSection(&g_mixer_cs);
}

APUMixerVoice *apu_mixer_get_voice(int slot)
{
    if (slot < 0 || slot >= APU_MIXER_MAX_VOICES) return NULL;
    return &g_mixer_voices[slot];
}

int apu_mixer_set_position(int slot, uint32_t byte_offset)
{
    if (slot < 0 || slot >= APU_MIXER_MAX_VOICES) return 0;
    EnterCriticalSection(&g_mixer_cs);
    APUMixerVoice *v = &g_mixer_voices[slot];
    uint32_t frame_bytes = v->num_channels * sizeof(int16_t);
    int valid = (v->num_channels == 1 || v->num_channels == 2) &&
                byte_offset < v->pcm_bytes &&
                byte_offset / frame_bytes < v->pcm_bytes / frame_bytes;
    if (valid) v->play_offset = ((uint64_t)(byte_offset / frame_bytes)) << 16;
    LeaveCriticalSection(&g_mixer_cs);
    return valid;
}

void apu_mixer_get_state(int slot, uint32_t *byte_offset, int *active, int *looping)
{
    if (byte_offset) *byte_offset = 0;
    if (active) *active = 0;
    if (looping) *looping = 0;
    if (slot < 0 || slot >= APU_MIXER_MAX_VOICES) return;
    EnterCriticalSection(&g_mixer_cs);
    APUMixerVoice *v = &g_mixer_voices[slot];
    if (byte_offset) *byte_offset = (uint32_t)(v->play_offset >> 16) *
                                    v->num_channels * sizeof(int16_t);
    if (active) *active = v->active;
    if (looping) *looping = v->active && v->looping;
    LeaveCriticalSection(&g_mixer_cs);
}

void apu_mixer_play(int slot, int looping)
{
    if (g_audio_muted) return;
    if (slot < 0 || slot >= APU_MIXER_MAX_VOICES) return;
    EnterCriticalSection(&g_mixer_cs);
    APUMixerVoice *v = &g_mixer_voices[slot];
    if (!v->pcm_data || (v->num_channels != 1 && v->num_channels != 2) ||
        v->pcm_bytes < v->num_channels * sizeof(int16_t)) {
        LeaveCriticalSection(&g_mixer_cs);
        return;
    }
    v->looping = looping;
    if (!v->active) InterlockedIncrement((volatile LONG *)&g_mixer_active_count);
    v->active = 1;

    static int play_log_count = 0;
    if (play_log_count < 20) {
        fprintf(stderr, "[APU-MIX] Play voice %d: %u bytes, %u ch, %u Hz, vol=%.2f, loop=%d\n",
                slot, v->pcm_bytes, v->num_channels, v->sample_rate, v->volume, looping);
        play_log_count++;
    }
    LeaveCriticalSection(&g_mixer_cs);

    /* The frame thread takes the APU lock before the mixer lock. This runs on
     * a guest thread, so it announces the wait -- see mcpx_apu_lock_guest. */
    extern MCPXAPUState *g_state;
    if (g_state) {
        mcpx_apu_lock_guest(g_state);
        g_state->pause_requested = false;
        qemu_cond_signal(&g_state->cond);
        mcpx_apu_unlock_guest(g_state);
    }
}

void apu_mixer_stop(int slot)
{
    if (slot < 0 || slot >= APU_MIXER_MAX_VOICES) return;
    EnterCriticalSection(&g_mixer_cs);
    if (g_mixer_voices[slot].active) {
        g_mixer_voices[slot].active = 0;
        InterlockedDecrement((volatile LONG *)&g_mixer_active_count);
    }
    LeaveCriticalSection(&g_mixer_cs);
}

/* Mix all active voices into frame_buf. Called from mcpx_apu_monitor_frame.
 * Keep 16 fractional bits in a wide offset so buffers can exceed 65536 frames. */
static void mixer_render(int16_t frame_buf[][2], int num_samples)
{
    if (!g_mixer_initialized) return;
    EnterCriticalSection(&g_mixer_cs);

    for (int v = 0; v < APU_MIXER_MAX_VOICES; v++) {
        APUMixerVoice *voice = &g_mixer_voices[v];
        if (!voice->active || !voice->pcm_data || voice->pcm_bytes == 0)
            continue;

        uint32_t total_frames = voice->pcm_bytes / sizeof(int16_t);
        if (voice->num_channels == 2) total_frames /= 2;
        if (total_frames == 0) continue;

        /* Fixed-point 16.16 increment per output sample */
        uint64_t inc = ((uint64_t)voice->sample_rate << 16) / 48000;
        uint64_t pos = voice->play_offset;
        uint64_t end = (uint64_t)total_frames << 16;
        float vol = voice->volume;

        for (int i = 0; i < num_samples; i++) {
            uint32_t src_frame = pos >> 16;

            if (src_frame >= total_frames) {
                if (voice->looping) {
                    pos %= end;
                    src_frame = (uint32_t)(pos >> 16);
                } else {
                    pos = 0;
                    voice->active = 0;
                    InterlockedDecrement((volatile LONG *)&g_mixer_active_count);
                    break;
                }
            }

            int32_t left, right;
            if (voice->num_channels >= 2) {
                left  = (int32_t)(voice->pcm_data[src_frame * 2] * vol);
                right = (int32_t)(voice->pcm_data[src_frame * 2 + 1] * vol);
            } else {
                left = right = (int32_t)(voice->pcm_data[src_frame] * vol);
            }

            /* Accumulate (mix) into frame_buf with clamping */
            int32_t mixed_l = frame_buf[i][0] + left;
            int32_t mixed_r = frame_buf[i][1] + right;
            if (mixed_l > 32767) mixed_l = 32767;
            if (mixed_l < -32768) mixed_l = -32768;
            if (mixed_r > 32767) mixed_r = 32767;
            if (mixed_r < -32768) mixed_r = -32768;
            frame_buf[i][0] = (int16_t)mixed_l;
            frame_buf[i][1] = (int16_t)mixed_r;

            pos += inc;
        }

        voice->play_offset = pos;
        uint32_t end_frame = pos >> 16;
        if (end_frame >= total_frames) {
            if (voice->looping) {
                voice->play_offset = pos % end;
            } else if (voice->active) {
                voice->play_offset = 0;
                voice->active = 0;
                InterlockedDecrement((volatile LONG *)&g_mixer_active_count);
            }
        }
    }
    LeaveCriticalSection(&g_mixer_cs);
}
