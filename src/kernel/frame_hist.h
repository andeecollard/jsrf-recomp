#ifndef FRAME_HIST_H
#define FRAME_HIST_H
/* PER-FRAME COST DISTRIBUTIONS, AND THE ARITHMETIC THAT COMPARES TWO OF THEM.
 *
 * Extracted from nv2a_pb_exec.c when [SYNC] became the second user. It was
 * inline there and correct; the reason it is a header now is that a SECOND
 * histogram whose samples are DERIVED from the first -- frame time minus that
 * frame's sync -- carries arithmetic that can be wrong in ways a total cannot
 * be, and this tree has retired nine instruments for lying. Pure functions in
 * a header are testable without a device, a game or a frame, which is what
 * G10 asks of every headline counter.
 *
 * A histogram rather than a running mean, because percentiles are the point.
 * 500 us bins to 128 ms and one overflow bucket: 2 KB of statics, one
 * clock_gettime and one increment per frame.
 *
 * Percentiles report their bin's UPPER edge, so p50=16.5 means "half of all
 * frames finished in 16.5 ms or less", accurate to the 0.5 ms bin width.
 * max_us is exact and is not binned. */
#define FRAME_BIN_US   500u
#define FRAME_BINS     257u          /* 0..128 ms, plus one overflow bucket */
/* 60 Hz, to the nanosecond the hardware actually runs at: the NV2A's 59.94 Hz
 * gives 16.68 ms, not the 16.67 ms that assuming exactly 60 would. */
#define FRAME_BUDGET_MS 16.68

typedef struct {
    unsigned long long n, total_us, bin[FRAME_BINS];
    unsigned long long max_us;
} FrameHist;

static inline void frame_hist_add(FrameHist *h, unsigned long long us)
{
    unsigned b = (unsigned)(us / FRAME_BIN_US);
    if (b >= FRAME_BINS) b = FRAME_BINS - 1u;
    h->bin[b]++;
    h->n++;
    h->total_us += us;
    if (us > h->max_us) h->max_us = us;
}

/* Upper edge of the bin the p-th percentile falls in, in milliseconds. The
 * overflow bucket has no upper edge, so it reports the bottom of the bucket
 * and the caller's max_us is what says how far past it the run actually got. */
static inline double frame_pct(const FrameHist *h, double p)
{
    unsigned long long want, seen = 0;
    unsigned i;
    if (!h->n) return 0.0;
    want = (unsigned long long)((double)h->n * p);
    if (want < 1) want = 1;
    for (i = 0; i < FRAME_BINS; ++i) {
        seen += h->bin[i];
        if (seen >= want)
            return (i + 1u == FRAME_BINS)
                 ? (double)(FRAME_BINS - 1u) * FRAME_BIN_US / 1000.0
                 : (double)(i + 1u) * FRAME_BIN_US / 1000.0;
    }
    return (double)(FRAME_BINS - 1u) * FRAME_BIN_US / 1000.0;
}

/* SPLIT ONE FRAME INTO "SYNC" AND "WHAT THE FRAME WOULD HAVE COST WITHOUT IT".
 *
 * The two numbers come from two different clocks -- CLOCK_MONOTONIC for the
 * frame interval, the backend's own for the sync total -- so the invariant
 * sync <= frame is a claim about where sync runs, not an identity. It holds
 * only while every sync happens on the flipping thread inside the interval it
 * is charged to. A sample that breaks it would silently produce a NEGATIVE
 * nosync, which in unsigned arithmetic is an 18-quintillion-microsecond frame
 * landing in the overflow bucket and dragging p99 to the ceiling.
 *
 * So clamp, and COUNT the clamp. `clamps == 0` is the positive control that
 * says the subtraction was legitimate on every frame; a non-zero reading
 * invalidates the percentiles rather than merely denting them, and the caller
 * prints it beside them for that reason. */
static inline void frame_hist_split(unsigned long long frame_us,
                             unsigned long long sync_us,
                             unsigned long long *sync_out,
                             unsigned long long *nosync_out,
                             unsigned long long *clamps)
{
    if (sync_us > frame_us) { if (clamps) ++*clamps; sync_us = frame_us; }
    *sync_out = sync_us;
    *nosync_out = frame_us - sync_us;
}

/* THE GO/NO-GO. Would the median frame fit in the budget if this cost were
 * free? Non-zero says the work is worth attempting, and it is an UPPER bound:
 * removing a stall removes the serialisation, not the GPU work behind it. A
 * zero is the decisive answer -- it says the stall is not what owns the
 * median frame, whatever its share of the mean. */
static inline int frame_hist_budget_ok(const FrameHist *nosync)
{
    return nosync->n && frame_pct(nosync, 0.50) <= FRAME_BUDGET_MS;
}
#endif
