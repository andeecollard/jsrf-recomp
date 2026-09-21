/* CAN THE PRODUCER WRITE OVER A FRAME A READER IS HOLDING?
 *
 * The snapshot taken at FLIP_STALL had one buffer and three readers on other
 * threads, and its own comment settled for the consequence: "The copy races
 * the reader and can tear a frame". It could do worse than tear -- the same
 * function reallocs that buffer when the surface geometry changes, and this
 * title flips between surfaces of differing size.
 *
 * So the property under test is not "frames look plausible", it is EVERY
 * FRAME HANDED OUT IS INTERNALLY CONSISTENT: its pixels are all from one
 * publish, its dimensions describe those pixels, and its storage is at least
 * that big. A producer running flat out against readers that hold their frame
 * for a while is the cheapest way to ask, and it needs no device and no game.
 *
 * Every byte of a frame carries a value derived from that frame's OWN
 * sequence number, so a reader that sees two different values in one frame has
 * caught the producer writing underneath it. Nothing here sleeps to make the
 * race likelier; the pool is small and the producer is unthrottled, which
 * makes slot reuse the common case rather than a rare one.
 *
 * WITH A NOTE ON WHAT A PASS MEANS. A race test that passes has not proved the
 * race is impossible. What makes this one worth having is that the version it
 * replaced FAILS it in well under a second -- verified when it was written --
 * so it discriminates, and the arithmetic it guards (dimensions published with
 * their pixels, storage sized before use) is checked exactly, not statistically.
 */
#include "../../src/kernel/frame_pool.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FRAMES   4000
#define READERS  3
/* Below this the readers cannot have exercised slot reuse, so a clean result
 * would mean nothing. It is a floor on the OBSERVATION, not on the work. */
#define MIN_SEEN 100

static FramePool pool;

/* ATOMIC, NOT `volatile`. volatile orders nothing between threads and is not a
 * synchronisation primitive; ThreadSanitizer says so, and it was right -- the
 * first version of this test raced on its own stop flag while testing the pool
 * for races. A test whose harness races cannot report cleanly on anything. */
static atomic_int producing = 1;

static unsigned long bad_pixels, bad_dims, bad_cap, seen, empties;
static pthread_mutex_t tally = PTHREAD_MUTEX_INITIALIZER;

/* Readers publish their running total here so the producer can wait for them.
 * Under the tally lock, because it is read from the producer thread. */
static unsigned long seen_live;

static unsigned long tally_seen(void)
{
    unsigned long v;
    pthread_mutex_lock(&tally);
    v = seen_live;
    pthread_mutex_unlock(&tally);
    return v;
}

/* The byte every pixel of frame `seq` is filled with. Odd multiplier so
 * consecutive frames never share a value. */
static unsigned char fill_of(unsigned long seq)
{
    return (unsigned char)(seq * 37u + 11u);
}

/* Three sizes, so the pool has to grow a slot and hand back frames whose
 * dimensions differ from the one published before them. */
static void size_of(unsigned long seq, unsigned *w, unsigned *h, unsigned *bpp)
{
    static const unsigned ws[3] = { 64, 96, 80 };
    static const unsigned hs[3] = { 48, 32, 60 };
    *w   = ws[seq % 3];
    *h   = hs[seq % 3];
    *bpp = (seq % 2) ? 4 : 2;
}

static void *producer(void *unused)
{
    unsigned long i;
    (void)unused;
    /* KEEP PUBLISHING UNTIL THE READERS HAVE ACTUALLY SEEN SOMETHING.
     *
     * The frame count alone is not the control. Under ThreadSanitizer the
     * readers run perhaps fifty times slower than the producer, so a fixed
     * 4000 frames left them with 70 -- below the "the race was never
     * exercised" floor, and the test failed for being too fast rather than
     * for being wrong. Publishing until BOTH bounds are met makes the control
     * independent of how the two sides are scheduled. */
    for (i = 1; i <= FRAMES || tally_seen() < MIN_SEEN; ++i) {
        unsigned w, h, bpp;
        FramePoolSlot *f;

        size_of(i, &w, &h, &bpp);
        f = frame_pool_begin(&pool, w, h, bpp);
        if (!f) continue;                    /* no free slot; pool counted it */

        /* Fill for the sequence number this frame is ABOUT to get. publishes
         * has already counted every frame that reached this point, so the
         * next seq is publishes+1 -- the pool hands them out in order and this
         * is the only producer. */
        memset(f->px, fill_of(pool.publishes + 1), (size_t)w * h * bpp);
        frame_pool_publish(&pool, f, w, h, bpp, 0x1000u + (unsigned)(i % 4));
    }
    atomic_store(&producing, 0);
    return NULL;
}

static void *reader(void *unused)
{
    unsigned long my_seen = 0, my_bad = 0, my_dims = 0, my_cap = 0, my_empty = 0;
    (void)unused;

    while (atomic_load(&producing)) {
        const FramePoolSlot *f = frame_pool_acquire(&pool);
        if (!f) { ++my_empty; continue; }

        {
            unsigned w = f->w, h = f->h, bpp = f->bpp, ew, eh, ebpp;
            unsigned long seq = f->seq;
            size_t len = (size_t)w * h * bpp;
            unsigned char want = fill_of(seq);
            size_t k;
            int torn = 0;

            ++my_seen;
            if ((my_seen & 7u) == 0) {      /* cheap: every eighth frame */
                pthread_mutex_lock(&tally);
                seen_live += 8;
                pthread_mutex_unlock(&tally);
            }

            /* The size this frame's sequence number says it should be. A
             * mismatch means dimensions and pixels came from different
             * publishes -- four separate loads off one shared buffer is how
             * that used to happen. */
            size_of(seq, &ew, &eh, &ebpp);
            if (w != ew || h != eh || bpp != ebpp) ++my_dims;

            if (len > f->cap) { ++my_cap; len = f->cap; }

            /* Read the WHOLE frame, slowly enough that the producer laps us.
             * Any byte that is not this frame's fill was written by another
             * publish into storage we are holding. */
            for (k = 0; k < len; ++k)
                if (f->px[k] != want) { torn = 1; break; }
            if (torn) ++my_bad;
        }
        frame_pool_release(&pool, f);
    }

    pthread_mutex_lock(&tally);
    seen += my_seen; bad_pixels += my_bad; bad_dims += my_dims;
    bad_cap += my_cap; empties += my_empty;
    pthread_mutex_unlock(&tally);
    return NULL;
}

int main(void)
{
    pthread_t p, r[READERS];
    int i, fail = 0;

    frame_pool_init(&pool);

    for (i = 0; i < READERS; ++i) pthread_create(&r[i], NULL, reader, NULL);
    pthread_create(&p, NULL, producer, NULL);

    pthread_join(p, NULL);
    for (i = 0; i < READERS; ++i) pthread_join(r[i], NULL);

    fprintf(stderr,
            "frame pool: %lu published, %lu dropped (no free slot),"
            " %lu frames read by %d readers, %lu empty acquires\n",
            pool.publishes, pool.dropped, seen, READERS, empties);

    /* THE POSITIVE CONTROL. A run where the readers saw a handful of frames
     * proves nothing about slot reuse, and a pool that never had to drop is
     * a pool the producer never pushed hard enough. */
    if (seen < MIN_SEEN) {
        fprintf(stderr, "FAIL: readers saw only %lu frames -- the race was"
                        " never exercised\n", seen);
        fail = 1;
    }
    if (pool.publishes < FRAMES / 2) {
        fprintf(stderr, "FAIL: only %lu of %d frames were published\n",
                pool.publishes, FRAMES);
        fail = 1;
    }

    if (bad_pixels) {
        fprintf(stderr, "FAIL: %lu frames had pixels from more than one"
                        " publish -- the producer wrote over a frame a reader"
                        " was holding\n", bad_pixels);
        fail = 1;
    }
    if (bad_dims) {
        fprintf(stderr, "FAIL: %lu frames carried dimensions that do not"
                        " match their own sequence number\n", bad_dims);
        fail = 1;
    }
    if (bad_cap) {
        fprintf(stderr, "FAIL: %lu frames were larger than the storage behind"
                        " them\n", bad_cap);
        fail = 1;
    }

    frame_pool_free(&pool);
    fprintf(stderr, "%s\n", fail ? "FAILED" : "ok");
    return fail;
}
