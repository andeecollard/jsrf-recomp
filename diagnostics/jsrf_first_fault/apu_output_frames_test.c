/* The engine must produce output frames even when no audio sink exists.
 *
 * WHY THIS TEST EXISTS. On 14 Sep 2026 a gameplay run read
 * [APU-PACE] gen_hz=0 batches=0 frames=0, which looks exactly like the
 * emulated sound engine having died, and an hour went into treating it as one.
 * It was the host: macOS CoreAudio refused the device
 * ("CoreAudio error (AudioQueueStart): -66681"), apu_sdl2_init failed, and the
 * APU fell back to a waveOut path that does nothing on this platform. Every
 * counter on that line comes from the SDL sink, so all three read zero while
 * the engine upstream was perfectly healthy. [APU-FRAME] se= did not help
 * either: it counts rendered subframes, not delivered ones, so it stayed
 * healthy too and made the contradiction look worse rather than clearer.
 *
 * ctest passed 24/26 throughout, because NO TEST IN THIS TREE HAD EVER DRIVEN
 * THE AUDIO OUTPUT PATH. That is the same shape of blindness as having no test
 * that retires a voice, which is how the APU trap storm survived for the life
 * of the project. This closes it for the output half.
 *
 * The invariant under test is the one that separates the two failures:
 * g_apu_out_frames is incremented in mcpx_apu_monitor_frame BEFORE a sink is
 * chosen, so it advances whether or not a device exists. out_hz near 48000
 * with gen_hz=0 means the device is dead and the emulation is fine; both at
 * zero means the engine stopped. No sound device is opened here -- that is the
 * point, and it is also what lets this run on a headless CI box.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "apu.h"
#include "apu_state.h"
#include "apu_regs.h"

typedef void (*recomp_func_t)(void);
recomp_func_t recomp_lookup(uint32_t va) { (void)va; return NULL; }
recomp_func_t recomp_lookup_manual(uint32_t va) { (void)va; return NULL; }
int xbox_VideoIsPlaying(void) { return 0; }

#define CHECK(x) do { if (!(x)) { \
    fprintf(stderr, "line %d: %s\n", __LINE__, #x); return 1; } } while (0)

extern unsigned long long g_apu_out_frames;

int main(void)
{
    /* Eight 32-sample subframes make one output frame; mcpx_apu_monitor_frame
     * returns early unless (ep_frame_div + 1) % 8 == 0, so the divider is part
     * of what is being asserted rather than something to work around. */
    const int per_frame = 8 * NUM_SAMPLES_PER_FRAME;
    MCPXAPUState *d = calloc(1, sizeof(*d));
    unsigned long long before;
    int i;

    CHECK(d);
    qemu_mutex_init(&d->lock);
    qemu_cond_init(&d->cond);

    /* No sink is initialised: no XAudio2, no SDL2 device, no waveOut. This is
     * precisely the state the wedged-CoreAudio run was in. */
    before = g_apu_out_frames;

    /* Seven subframes short of a frame boundary must produce nothing. Without
     * this half the test would pass against a monitor_frame that ignored the
     * divider and submitted eight times too often. */
    for (i = 0; i < 7; i++) {
        d->ep_frame_div = i;
        mcpx_apu_monitor_frame(d);
    }
    CHECK(g_apu_out_frames == before);

    /* The eighth completes it. */
    d->ep_frame_div = 7;
    mcpx_apu_monitor_frame(d);
    CHECK(g_apu_out_frames == before + (unsigned long long)per_frame);

    /* And it keeps producing: three more frames, each exactly one frame apart.
     * A path that produced once and then stalled -- which is what the log
     * looked like during the incident -- would fail here. */
    for (i = 0; i < 3; i++) {
        d->ep_frame_div = 7;
        mcpx_apu_monitor_frame(d);
    }
    CHECK(g_apu_out_frames == before + (unsigned long long)per_frame * 4);

    free(d);
    printf("apu output frames: OK (%d samples per frame, 4 frames, no sink)\n",
           per_frame);
    return 0;
}
