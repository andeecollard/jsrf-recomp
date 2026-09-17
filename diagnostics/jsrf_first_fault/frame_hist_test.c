/* CAN THE SYNC HISTOGRAM SAY "NO"?
 *
 * [SYNC]/[NOSYNC] exists to decide one thing: whether removing the surface
 * swap's stall can put the MEDIAN frame inside the 16.68 ms budget. A week
 * went into a flip-sync redesign on the strength of a cumulative average that
 * was true and did not answer that question, so the instrument replacing it
 * has to be shown capable of BOTH answers before either is believed.
 *
 * That is G10's shape, and it is the same shape jsrf_ff_refusal uses: drive
 * the thing into the state it is supposed to detect and assert it detects it,
 * then into the healthy state and assert it does not. A verdict line that
 * always reads "reachable" is not an instrument, it is a comment.
 *
 * Three properties, none of which needs a device, a game or a frame:
 *   1. The percentile is a percentile -- the same arithmetic [FRAME] has
 *      reported since it was written, now shared rather than duplicated.
 *   2. The per-frame split clamps a sync longer than its frame AND counts the
 *      clamp, because an unsigned underflow there would post an
 *      18-quintillion-microsecond frame and wreck every percentile above it.
 *   3. The verdict flips. Sync on the median frame -> "reachable"; the same
 *      total sync concentrated in the tail -> "NOT reachable".
 *
 * Property 3 is the one that matters. Both runs below spend the SAME total
 * time in sync; they differ only in which frames it lands on, which is exactly
 * the distinction a mean cannot make and the reason this file exists.
 */
#include "frame_hist.h"
#include <stdio.h>
#include <string.h>

static int failures;

#define CHECK(cond, ...) do { if (!(cond)) { \
    fprintf(stderr, "%s:%d: ", __FILE__, __LINE__); \
    fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); ++failures; } } while (0)

/* Feed one run of `n` frames through the same split the flip uses, and report
 * what the two histograms would say. */
static void run_frames(const unsigned long long *frame_us,
                       const unsigned long long *sync_us, unsigned n,
                       FrameHist *sync, FrameHist *nosync,
                       unsigned long long *clamps)
{
    unsigned i;
    for (i = 0; i < n; ++i) {
        unsigned long long s, ns;
        frame_hist_split(frame_us[i], sync_us[i], &s, &ns, clamps);
        frame_hist_add(sync, s);
        frame_hist_add(nosync, ns);
    }
}

static void percentiles(void)
{
    FrameHist h;
    unsigned i;
    memset(&h, 0, sizeof h);

    /* An empty histogram must not answer. p50=0.0 out of no samples reads
     * like "every frame was instant", which is how an absent measurement
     * becomes a performance claim. */
    CHECK(frame_pct(&h, 0.50) == 0.0, "an empty histogram invented a percentile");
    CHECK(!frame_hist_budget_ok(&h),
          "an empty histogram reported the budget REACHABLE -- a run that "
          "measured nothing would read as a run that passed");

    /* 100 frames at exactly 10.0 ms. Every percentile is the upper edge of
     * that bin: 10.0 ms lands at the top of bin 20, whose edge is 10.5. */
    for (i = 0; i < 100; ++i) frame_hist_add(&h, 10000);
    CHECK(h.n == 100, "n=%llu after 100 adds", (unsigned long long)h.n);
    CHECK(h.max_us == 10000, "max_us=%llu", (unsigned long long)h.max_us);
    CHECK(frame_pct(&h, 0.50) == 10.5, "p50=%.1f of a constant 10 ms run",
          frame_pct(&h, 0.50));
    CHECK(frame_pct(&h, 0.99) == 10.5, "p99=%.1f of a constant 10 ms run",
          frame_pct(&h, 0.99));

    /* Two 80 ms frames in 100 must move p99 and leave p50 alone. That is the
     * whole reason this is a histogram and not a mean.
     *
     * TWO, not one. With a single slow frame in 100 the 99th sample is still a
     * fast one, so p99 correctly reads 10.5 and the slow frame shows up only
     * in max_us -- which is a property of what a percentile MEANS, not a bug,
     * and asserting otherwise is how a good instrument gets "fixed" into a
     * lying one. It cost this test one iteration to learn. */
    memset(&h, 0, sizeof h);
    for (i = 0; i < 98; ++i) frame_hist_add(&h, 10000);
    frame_hist_add(&h, 80000);
    frame_hist_add(&h, 80000);
    CHECK(frame_pct(&h, 0.50) == 10.5, "two slow frames moved p50 to %.1f",
          frame_pct(&h, 0.50));
    CHECK(frame_pct(&h, 0.99) >= 80.0, "p99=%.1f missed the 80 ms frames",
          frame_pct(&h, 0.99));
    CHECK(h.max_us == 80000, "max_us=%llu missed the 80 ms frames",
          (unsigned long long)h.max_us);

    /* And the single-slow-frame case, asserted as the behaviour it is, so a
     * later reader does not "correct" it. */
    memset(&h, 0, sizeof h);
    for (i = 0; i < 99; ++i) frame_hist_add(&h, 10000);
    frame_hist_add(&h, 80000);
    CHECK(frame_pct(&h, 0.99) == 10.5,
          "p99=%.1f: with one slow frame in 100, the 99th sample is fast",
          frame_pct(&h, 0.99));
    CHECK(h.max_us == 80000, "max_us=%llu -- the one slow frame must survive "
          "in the un-binned maximum even when no percentile reaches it",
          (unsigned long long)h.max_us);

    /* Past the last bin everything lands in the overflow bucket, and max_us
     * -- which is not binned -- is what still says how far past. */
    memset(&h, 0, sizeof h);
    frame_hist_add(&h, 500000);
    CHECK(h.bin[FRAME_BINS - 1u] == 1, "a 500 ms frame missed the overflow bucket");
    CHECK(h.max_us == 500000, "max_us=%llu lost the exact 500 ms",
          (unsigned long long)h.max_us);
}

static void split(void)
{
    unsigned long long s, ns, clamps = 0;

    /* HEALTHY: sync is part of the frame, so the two halves add back up. */
    frame_hist_split(18000, 8000, &s, &ns, &clamps);
    CHECK(s == 8000 && ns == 10000, "split(18,8) gave sync=%llu nosync=%llu",
          (unsigned long long)s, (unsigned long long)ns);
    CHECK(clamps == 0, "the clamp counter fired on a HEALTHY frame: it cannot "
          "tell the two apart, so a zero reading from it means nothing");

    /* Sync is the whole frame. Still healthy, still adds up, still no clamp:
     * the boundary case must not be counted as a violation or the control
     * reads non-zero on a perfectly good run. */
    frame_hist_split(18000, 18000, &s, &ns, &clamps);
    CHECK(s == 18000 && ns == 0, "split(18,18) gave sync=%llu nosync=%llu",
          (unsigned long long)s, (unsigned long long)ns);
    CHECK(clamps == 0, "an exactly-equal sync was counted as a clamp");

    /* THE DEFECT: sync longer than the frame it is charged to. Unclamped this
     * is 18446744073709.55 ms of "nosync". */
    frame_hist_split(18000, 20000, &s, &ns, &clamps);
    CHECK(s == 18000, "an over-long sync was not clamped: sync=%llu",
          (unsigned long long)s);
    CHECK(ns == 0, "an over-long sync underflowed nosync to %llu -- every "
          "percentile downstream of this is ruined", (unsigned long long)ns);
    CHECK(clamps == 1, "the clamp was performed but NOT counted, so the run "
          "would print ruined percentiles with a clean positive control");
}

/* THE VERDICT HAS TO BE ABLE TO GO BOTH WAYS.
 *
 * Two 100-frame runs, both 20 ms mean, both spending 400 ms total in sync --
 * identical in every cumulative statistic the old instrument could report.
 * They differ only in WHERE the sync lands, and the verdict must separate
 * them. If it cannot, "48% of the frame" was never evidence for anything. */
static void verdict(void)
{
    FrameHist sync, nosync;
    unsigned long long frame[100], sy[100], clamps = 0;
    unsigned i;

    /* ON THE MEDIAN FRAME: every frame 20 ms with 4 ms of sync. Removing it
     * leaves 16 ms, inside the budget -- the work is worth doing. */
    memset(&sync, 0, sizeof sync); memset(&nosync, 0, sizeof nosync);
    for (i = 0; i < 100; ++i) { frame[i] = 20000; sy[i] = 4000; }
    run_frames(frame, sy, 100, &sync, &nosync, &clamps);
    CHECK(frame_hist_budget_ok(&nosync),
          "sync on EVERY frame, 16 ms without it, and the verdict still said "
          "the budget was out of reach (NOSYNC p50=%.1f)",
          frame_pct(&nosync, 0.50));

    /* IN THE TAIL: the same 400 ms of sync, all of it on 5 frames, and the
     * other 95 frames are 20 ms of something else entirely. Removing the
     * stall cannot touch the median, and the verdict has to say so. */
    memset(&sync, 0, sizeof sync); memset(&nosync, 0, sizeof nosync);
    for (i = 0; i < 100; ++i) { frame[i] = 20000; sy[i] = 0; }
    for (i = 0; i < 5; ++i)   { frame[i] = 100000; sy[i] = 80000; }
    run_frames(frame, sy, 100, &sync, &nosync, &clamps);
    CHECK(!frame_hist_budget_ok(&nosync),
          "all of the sync was in the tail and the median frame was 20 ms of "
          "other work, but the verdict said removing sync would reach 60 fps "
          "(NOSYNC p50=%.1f) -- it cannot say no, so its yes means nothing",
          frame_pct(&nosync, 0.50));

    /* And the two runs really were indistinguishable to a mean, which is the
     * claim that justifies replacing one with the other. */
    CHECK(sync.total_us == 400000, "the tail run's total sync was %llu us, "
          "not the 400000 the median run spent -- the two arms are not "
          "comparable and this test proves nothing",
          (unsigned long long)sync.total_us);
    CHECK(clamps == 0, "a synthetic run with sync <= frame everywhere still "
          "clamped %llu frames", (unsigned long long)clamps);
}

int main(void)
{
    percentiles();
    split();
    verdict();
    if (failures) {
        fprintf(stderr, "frame_hist_test: %d failure(s)\n", failures);
        return 1;
    }
    printf("frame_hist_test: ok\n");
    return 0;
}
