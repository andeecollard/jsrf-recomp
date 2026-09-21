#ifndef FRAME_POOL_H
#define FRAME_POOL_H
/* PUBLISHING A FINISHED FRAME TO A READER ON ANOTHER THREAD.
 *
 * The snapshot taken at NV097_FLIP_STALL had ONE buffer, written by the pusher
 * thread and read by three others -- the presenter's hook, the report timer's
 * BMP dump and the blank-frame counter. Its comment admitted the consequence
 * and called it acceptable:
 *
 *     "The copy races the reader and can tear a frame; neither side may block
 *      the other, and a torn frame is a far smaller artefact than the flicker
 *      it replaces."
 *
 * A tear was not the whole cost. The same function REALLOCS that buffer when
 * the surface geometry changes, so a reader mid-memcpy can be reading freed
 * storage -- and the dimensions it was given arrive as four separate loads, so
 * it can also read a new frame's width against an old frame's height. The
 * title flips between several colour surfaces of differing size, which is
 * exactly when that happens.
 *
 * THE FIX IS OWNERSHIP, NOT ATOMICITY. Pixels, dimensions, the surface the
 * copy came from and the sequence number are ONE object here, published as
 * one, and a slot is reused only once no reader holds it. Neither side blocks
 * on the other: the producer takes a free slot or drops the frame (counted),
 * and a reader takes whatever is published or nothing.
 *
 * SLOT COUNT. One published, one the producer is filling, and one per
 * concurrent reader -- the presenter and the dumper -- plus the frame the
 * producer still references for its own same-thread readers. Five covers that
 * with a slot to spare; `dropped` says if it ever did not.
 *
 * Header-only and pthread-only so the invariant can be tested without a
 * device, a game or a frame: see frame_pool_test.c, which runs a producer flat
 * out against readers and checks every frame it hands out is internally
 * consistent. That test fails against the single-buffer version. */
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#ifndef FRAME_POOL_SLOTS
#define FRAME_POOL_SLOTS 5
#endif

/* For a caller that holds one frame per thread. Spelled here rather than
 * borrowed from xbox_memory_layout.h so this header stays free-standing and
 * its test needs nothing but pthreads. */
#ifndef FRAME_POOL_TLS
#if defined(_MSC_VER)
#  define FRAME_POOL_TLS __declspec(thread)
#elif defined(__GNUC__) || defined(__clang__)
#  define FRAME_POOL_TLS __thread
#else
#  define FRAME_POOL_TLS _Thread_local
#endif
#endif

typedef struct {
    unsigned char *px;
    size_t         cap;          /* bytes allocated at px */
    unsigned       w, h, bpp;
    unsigned       offset;       /* the colour surface the copy came from */
    unsigned long  seq;          /* ++ per publish; 0 means never published */
    int            readers;      /* consumers holding it */
    int            writing;      /* the producer is filling it */
} FramePoolSlot;

typedef struct {
    FramePoolSlot   slot[FRAME_POOL_SLOTS];
    int             published;   /* index, or -1 before the first publish */
    unsigned long   seq;
    unsigned long   publishes;
    unsigned long   dropped;     /* publishes abandoned for want of a slot */
    unsigned long   acquires;
    unsigned long   empty;       /* acquires that found nothing published */
    pthread_mutex_t m;
    int             ready;
} FramePool;

/* A STATIC POOL INITIALISES ITSELF, because the lazy path below is racy on
 * exactly the call that matters. The producer reaches frame_pool_begin and the
 * presenter reaches frame_pool_acquire, on two threads, and whichever arrives
 * first would run the lazy init -- so both can run it, memset the pool out
 * from under the other and pthread_mutex_init the same mutex twice. It is a
 * startup-only window and it is still a window. A pool that is a static
 * carries its own initial state instead:
 *
 *     static FramePool s_pool = FRAME_POOL_INIT;
 *
 * frame_pool_init then finds ready == 1 and does nothing, on every path. */
#define FRAME_POOL_INIT { .published = -1, .m = PTHREAD_MUTEX_INITIALIZER, \
                          .ready = 1 }

/* For a pool that is NOT a static: call it once, before any other thread can
 * reach the pool. It is not safe to race. */
static inline void frame_pool_init(FramePool *p)
{
    if (p->ready) return;
    memset(p, 0, sizeof *p);
    pthread_mutex_init(&p->m, NULL);
    p->published = -1;
    p->ready = 1;
}

/* A slot to fill, sized for w*h*bpp, or NULL when every slot is spoken for.
 * The slot is marked `writing` and cannot be published or handed to a reader
 * until frame_pool_publish or frame_pool_abandon says so. */
static inline FramePoolSlot *frame_pool_begin(FramePool *p,
                                              unsigned w, unsigned h,
                                              unsigned bpp)
{
    size_t need = (size_t)w * h * bpp;
    FramePoolSlot *s = NULL;
    int i;

    if (!w || !h || !bpp) return NULL;
    frame_pool_init(p);
    pthread_mutex_lock(&p->m);
    for (i = 0; i < FRAME_POOL_SLOTS; ++i) {
        FramePoolSlot *c = &p->slot[i];
        if (c->readers == 0 && !c->writing && i != p->published) { s = c; break; }
    }
    if (!s) {
        ++p->dropped;
        pthread_mutex_unlock(&p->m);
        return NULL;
    }
    s->writing = 1;
    pthread_mutex_unlock(&p->m);

    /* Outside the lock: nobody else may touch a slot marked `writing`, and it
     * had no readers when it was chosen, so the realloc cannot pull storage
     * out from under one. That is the failure the single buffer had. */
    if (s->cap < need) {
        unsigned char *n = (unsigned char *)realloc(s->px, need);
        if (!n) {
            pthread_mutex_lock(&p->m);
            s->writing = 0;
            ++p->dropped;
            pthread_mutex_unlock(&p->m);
            return NULL;
        }
        s->px  = n;
        s->cap = need;
    }
    return s;
}

/* The frame is complete: give it a sequence number and make it the one a
 * reader gets. Dimensions are set HERE, not in frame_pool_begin, so nothing
 * published ever carries a size its pixels were not written at. */
static inline void frame_pool_publish(FramePool *p, FramePoolSlot *s,
                                      unsigned w, unsigned h, unsigned bpp,
                                      unsigned offset)
{
    int i;
    if (!s) return;
    /* THE ONE WAY A CALLER CAN STILL PUBLISH A LIE: pass frame_pool_publish
     * dimensions that frame_pool_begin was not asked to size for. Every reader
     * computes w*h*bpp and indexes px with it, so that would hand out a frame
     * that reads past its own storage. Refuse it here rather than trust the
     * two call sites to stay in step. */
    if ((size_t)w * h * bpp > s->cap) {
        pthread_mutex_lock(&p->m);
        s->writing = 0;
        ++p->dropped;
        pthread_mutex_unlock(&p->m);
        return;
    }
    pthread_mutex_lock(&p->m);
    s->w = w; s->h = h; s->bpp = bpp;
    s->offset = offset;
    s->seq = ++p->seq;
    s->writing = 0;
    for (i = 0; i < FRAME_POOL_SLOTS; ++i)
        if (&p->slot[i] == s) { p->published = i; break; }
    ++p->publishes;
    pthread_mutex_unlock(&p->m);
}

/* Give a begun slot back without publishing it -- the copy could not be
 * completed. The frame that was published stays published. */
static inline void frame_pool_abandon(FramePool *p, FramePoolSlot *s)
{
    if (!s) return;
    pthread_mutex_lock(&p->m);
    s->writing = 0;
    pthread_mutex_unlock(&p->m);
}

/* The published frame, held until the caller releases it. NULL before the
 * first publish. */
static inline const FramePoolSlot *frame_pool_acquire(FramePool *p)
{
    FramePoolSlot *s = NULL;
    frame_pool_init(p);
    pthread_mutex_lock(&p->m);
    ++p->acquires;
    if (p->published >= 0) {
        s = &p->slot[p->published];
        ++s->readers;
    } else {
        ++p->empty;
    }
    pthread_mutex_unlock(&p->m);
    return s;
}

static inline void frame_pool_release(FramePool *p, const FramePoolSlot *s)
{
    if (!s) return;
    pthread_mutex_lock(&p->m);
    {
        FramePoolSlot *m = (FramePoolSlot *)s;
        if (m->readers > 0) --m->readers;
    }
    pthread_mutex_unlock(&p->m);
}

/* Swap one held reference for the frame published now, for a caller whose API
 * has no release call: it holds at most one frame, and asking for the next one
 * is what gives the last one back. `held` is in/out. */
static inline const FramePoolSlot *frame_pool_reacquire(
        FramePool *p, const FramePoolSlot **held)
{
    const FramePoolSlot *next = frame_pool_acquire(p);
    if (*held) frame_pool_release(p, *held);
    *held = next;
    return next;
}

static inline void frame_pool_free(FramePool *p)
{
    int i;
    if (!p->ready) return;
    for (i = 0; i < FRAME_POOL_SLOTS; ++i) {
        free(p->slot[i].px);
        p->slot[i].px = NULL;
        p->slot[i].cap = 0;
    }
    p->published = -1;
}

#endif /* FRAME_POOL_H */
