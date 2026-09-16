/* The APU's output clock, tested as arithmetic.
 *
 * An EP frame is 256 samples at 48 kHz: 5333 AND ONE THIRD microseconds.
 * EP_FRAME_US is the truncation, and for the life of this file throttle()
 * added it bare -- pacing the engine at 256/0.005333 = 48003.0 Hz against a
 * device consuming at 48000. Sixty-two parts per million, one way, for ever.
 *
 * IT WAS MEASURED ALL ALONG AND NOBODY READ IT. Every run prints
 * `[APU-PACE] out_hz=`; across build-macos it reads 48003 or 48004 in
 * thousands of lines and 48000 in a handful, and `queued=` climbs
 * monotonically inside a single run -- 6144 bytes to 10240 over 200 s, about
 * 21 ms of audio latency added and never given back. Long enough and
 * apu_sdl2.c flushes the whole queue at APU_MAX_QUEUE_BYTES, which is the
 * audible jump at the end of a long session. The symptom was reported as
 * "the sound goes out of sync over the course of a run". It was this.
 *
 * A defect this size is invisible to any test that allows a tolerance, so this
 * one allows none: the period is checked for exactness over a whole second and
 * over an hour, which is the only way a one-third-microsecond error shows up.
 * A run confirms it in the field (out_hz reads 48000 now); this stops it
 * regressing without one.
 */
#include <stdio.h>
#include <stdint.h>

int64_t mcpx_apu_ep_frame_advance(int64_t next_us, int *frac);

static int failures;
#define CHECK(c) do { if (!(c)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); failures++; } \
} while (0)

int main(void)
{
    /* 1. Three frames are exactly 16000 us, which is 768 samples at 48 kHz.
     * This is the whole fix: the third of a microsecond has to come back. */
    {
        int frac = 0;
        int64_t t = mcpx_apu_ep_frame_advance(0, &frac);
        t = mcpx_apu_ep_frame_advance(t, &frac);
        t = mcpx_apu_ep_frame_advance(t, &frac);
        CHECK(t == 16000);
        CHECK(frac == 0);              /* and it hands back a clean state */
    }

    /* 2. No individual frame is allowed to wander. Each is 5333 or 5334 and
     * nothing else -- a scheme that paid the debt back in one lump every
     * hundred frames would satisfy test 1 and audibly judder. */
    {
        int frac = 0, i;
        int64_t t = 0, prev = 0;
        int n5333 = 0, n5334 = 0;
        for (i = 0; i < 300; ++i) {
            t = mcpx_apu_ep_frame_advance(t, &frac);
            int64_t d = t - prev;
            prev = t;
            if (d == 5333) n5333++;
            else if (d == 5334) n5334++;
            else CHECK(!"frame period is neither 5333 nor 5334");
        }
        CHECK(n5333 == 200);
        CHECK(n5334 == 100);
    }

    /* 3. One second of frames is one second, and one hour is one hour.
     * 187.5 EP frames/s, so 187500 frames is exactly 1000 seconds. At the old
     * truncated period this lands 62500 us short -- 62.5 ms of audio a
     * viewer's ear cannot hear but a queue counts. */
    {
        int frac = 0, i;
        int64_t t = 0;
        for (i = 0; i < 187500; ++i) t = mcpx_apu_ep_frame_advance(t, &frac);
        CHECK(t == 1000LL * 1000000LL);
        CHECK(t != 187500LL * 5333LL);   /* what it used to be */
    }

    /* 4. And therefore the generated rate is 48000.000000 Hz, not 48003.
     * Stated in the units the report prints, so a regression here and a
     * regression in [APU-PACE] out_hz say the same number. */
    {
        int frac = 0, i;
        int64_t t = 0;
        const int frames = 187500;         /* 1000 s worth */
        for (i = 0; i < frames; ++i) t = mcpx_apu_ep_frame_advance(t, &frac);
        double hz = (double)frames * 256.0 / ((double)t / 1000000.0);
        CHECK(hz == 48000.0);
    }

    if (failures) {
        fprintf(stderr, "apu_pace_test: %d failure(s)\n", failures);
        return 1;
    }
    printf("apu_pace_test: ok\n");
    return 0;
}
