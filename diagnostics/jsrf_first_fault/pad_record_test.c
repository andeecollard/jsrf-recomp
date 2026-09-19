/* The record -> replay round trip, at the format level.
 *
 * WHY THIS TEST CARRIES MORE THAN ITS USUAL WEIGHT. The recorder exists so
 * that the player's next crash stops costing a human fifteen minutes per
 * hypothesis, and until somebody plays a session there is no run behind any
 * claim about it. Everything that can be settled without the game is settled
 * here: the grammar, the run decomposition, the per-frame equality of a
 * replay against the session it came from, the checkpoint hash that reports
 * a divergence, and the header that refuses a recording taken against other
 * code. What is left over -- whether the guest's frame counter advances the
 * way a live session needs it to -- is named at the bottom of this file and
 * in the handover, not quietly assumed.
 *
 * THE THREE TRAPS THIS IS SHAPED AROUND, all of them real in this tree:
 *
 *   MORE THAN ONE POLL PER FRAME, AND FRAMES WITH NONE. The pad is polled on
 *   the guest's USB schedule (measured 118-124 Hz) while the frame rate has
 *   been seen anywhere from 24 to 202 fps, so both happen constantly. The
 *   sequences below deliberately vary the polls per frame from 0 to 3.
 *
 *   A SILENTLY TRUNCATED SCHEDULE. The old parser stopped at 256 events and
 *   said nothing; a recording is tens of thousands. The long sequence here is
 *   past that cap on purpose.
 *
 *   A NAME TABLE THAT IS NOT IN REPORT ORDER. bAnalogButtons is A B X Y Black
 *   White LT RT and the name table is A B X Y White Black LT RT, so a writer
 *   that walked the bytes would record every Black press as WHITE. The
 *   pseudo-random states exercise all eight.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "xinput_xbox.h"

static int failures;

#define CHECK(cond, ...)                                                   \
    do {                                                                   \
        if (!(cond)) {                                                     \
            failures++;                                                    \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);           \
            fprintf(stderr, __VA_ARGS__);                                  \
            fprintf(stderr, "\n");                                         \
        }                                                                  \
    } while (0)

/* A deterministic stand-in for a person holding a controller: mostly holding
 * one thing for a while, occasionally changing, with the sticks moving in
 * small steps rather than jumping, because a run decomposition is only
 * interesting when runs have length. */
static unsigned g_rng = 12345u;
static unsigned rnd(void) { g_rng = g_rng * 1103515245u + 12345u; return g_rng >> 8; }

static void make_sequence(XBOX_INPUT_STATE *seq, unsigned long frames)
{
    XBOX_INPUT_STATE cur;
    unsigned long f;
    unsigned long hold = 0;
    memset(&cur, 0, sizeof cur);
    for (f = 0; f < frames; f++) {
        if (hold == 0) {
            memset(&cur.Gamepad, 0, sizeof cur.Gamepad);
            hold = 1 + rnd() % 12;
            if (rnd() % 3) {
                cur.Gamepad.wButtons = (WORD)(rnd() & 0x00FFu);
                cur.Gamepad.bAnalogButtons[rnd() % 8] = (BYTE)(rnd() & 0xFFu);
                cur.Gamepad.bAnalogButtons[rnd() % 8] = (BYTE)(rnd() & 0xFFu);
                cur.Gamepad.sThumbLX = (SHORT)(int)((rnd() % 65536u) - 32768u);
                cur.Gamepad.sThumbLY = (SHORT)(int)((rnd() % 65536u) - 32768u);
                cur.Gamepad.sThumbRX = (SHORT)(int)((rnd() % 65536u) - 32768u);
                cur.Gamepad.sThumbRY = (SHORT)(int)((rnd() % 65536u) - 32768u);
            }
        }
        hold--;
        seq[f] = cur;
    }
}

static int gamepad_eq(const XBOX_GAMEPAD *a, const XBOX_GAMEPAD *b)
{
    return a->wButtons == b->wButtons
        && !memcmp(a->bAnalogButtons, b->bAnalogButtons, 8)
        && a->sThumbLX == b->sThumbLX && a->sThumbLY == b->sThumbLY
        && a->sThumbRX == b->sThumbRX && a->sThumbRY == b->sThumbRY;
}

static void describe(const XBOX_GAMEPAD *g, char *out, size_t n)
{
    snprintf(out, n, "btn=%04X a=%u b=%u x=%u y=%u bl=%u wh=%u lt=%u rt=%u "
             "lx=%d ly=%d rx=%d ry=%d",
             (unsigned)g->wButtons, g->bAnalogButtons[0], g->bAnalogButtons[1],
             g->bAnalogButtons[2], g->bAnalogButtons[3], g->bAnalogButtons[4],
             g->bAnalogButtons[5], g->bAnalogButtons[6], g->bAnalogButtons[7],
             (int)g->sThumbLX, (int)g->sThumbLY,
             (int)g->sThumbRX, (int)g->sThumbRY);
}

/* Polls per frame, cycling 1,2,0,1,3,... so the record path sees every shape
 * a live session produces. The LAST poll of a frame is that frame's state,
 * and a frame with no poll inherits the previous one -- which is exactly what
 * an uncovered frame does on replay, and is the property being checked. */
static unsigned polls_for_frame(unsigned long f)
{
    static const unsigned pattern[] = { 1, 2, 0, 1, 3, 1, 0, 1 };
    return pattern[f % (sizeof pattern / sizeof pattern[0])];
}

static unsigned long g_anchor_frame;                 /* drives the anchor */
static unsigned long test_anchor(void) { return 0x0102u << 16 | (g_anchor_frame / 60u); }

static void record_sequence(const char *path, const XBOX_INPUT_STATE *seq,
                            unsigned long frames)
{
    unsigned long f;
    CHECK(xbox_PadRecordOpen(path), "could not open %s for recording", path);
    for (f = 0; f < frames; f++) {
        unsigned p, n = polls_for_frame(f);
        g_anchor_frame = f;
        for (p = 0; p < n; p++)
            xbox_PadRecordSampleAtFrame(&seq[f], f);
    }
    xbox_PadRecordClose();
}

/* The state a replay OUGHT to produce for each frame: the last poll of the
 * most recent frame that had one. */
static void expected_sequence(const XBOX_INPUT_STATE *seq, unsigned long frames,
                              XBOX_INPUT_STATE *out)
{
    XBOX_INPUT_STATE held;
    unsigned long f;
    memset(&held, 0, sizeof held);
    for (f = 0; f < frames; f++) {
        if (polls_for_frame(f))
            held = seq[f];
        out[f] = held;
    }
}

int main(void)
{
    const unsigned long FRAMES = 20000;  /* thousands of runs: well past the
                                          * old 256-event cap, and enough for
                                          * a dozen checkpoints */
    XBOX_INPUT_STATE *seq = calloc(FRAMES, sizeof *seq);
    XBOX_INPUT_STATE *want = calloc(FRAMES, sizeof *want);
    char path[512], spec[520];
    unsigned long f, mismatches = 0;
    unsigned long long rec_hash;
    int ck_ok = 0, ck_bad = 0, state_bad = 0, loaded;
    unsigned long at = 0;

    if (!seq || !want) { fprintf(stderr, "out of memory\n"); return 1; }

    snprintf(path, sizeof path, "/tmp/jsrf_padrec_test_%d.padrec", (int)getpid());
    snprintf(spec, sizeof spec, "@%s", path);

    xbox_PadRecordSetIdentity("test-build", "test-gen");
    xbox_PadRecordSetAnchorFn(test_anchor);
    make_sequence(seq, FRAMES);
    expected_sequence(seq, FRAMES, want);

    /* ── 1. record ──────────────────────────────────────────────────── */
    record_sequence(path, seq, FRAMES);
    rec_hash = xbox_PadRecordHash();
    CHECK(rec_hash != 0, "the recorder produced no hash at all");

    /* ── 2. replay, and compare every single frame ──────────────────── */
    loaded = xbox_PadScriptLoad(spec);
    CHECK(loaded > 256, "only %d event(s) loaded -- the 256-event cap that"
          " silently truncated hand-written schedules is still capping"
          " recordings", loaded);
    /* <= FRAMES, not < : a run is folded into the checkpoint hash when the
     * replay passes its END, so the last run needs one step past the last
     * frame before the hashes can be compared. A live run gets that for free
     * by continuing to play. */
    for (f = 0; f <= FRAMES; f++) {
        XBOX_INPUT_STATE got;
        memset(&got, 0, sizeof got);
        g_anchor_frame = f;
        xbox_PadScriptApplyAtFrame(&got, f);
        if (f == FRAMES) break;
        if (!gamepad_eq(&got.Gamepad, &want[f].Gamepad)) {
            if (mismatches < 5) {
                char a[256], b[256];
                describe(&want[f].Gamepad, a, sizeof a);
                describe(&got.Gamepad, b, sizeof b);
                fprintf(stderr, "FAIL frame %lu:\n  recorded %s\n  replayed %s\n",
                        f, a, b);
            }
            mismatches++;
        }
    }
    CHECK(mismatches == 0, "%lu of %lu frames replayed differently from the"
          " session they were recorded from", mismatches, FRAMES);

    /* ── 3. the checkpoints agree, which is what a live run reads ───── */
    xbox_PadReplayStatus(&ck_ok, &ck_bad, &at, &state_bad);
    CHECK(ck_ok > 0, "no checkpoint was ever compared, so the divergence"
          " report in a live run would be vacuous");
    CHECK(ck_bad == 0, "%d checkpoint(s) diverged on a replay of the file the"
          " recorder had just written", ck_bad);
    CHECK(state_bad == 0, "%d state-anchor mismatch(es) against the recorder's"
          " own anchors", state_bad);
    CHECK(xbox_PadReplayHash() == rec_hash,
          "replay hash %016llx != recorder hash %016llx",
          (unsigned long long)xbox_PadReplayHash(), (unsigned long long)rec_hash);

    /* ── 4. a mismatched binary is REFUSED, not silently replayed ───── */
    xbox_PadScriptReset();
    xbox_PadRecordSetIdentity("SOME-OTHER-BUILD", "test-gen");
    loaded = xbox_PadScriptLoad(spec);
    CHECK(loaded == -1, "a recording taken against another build loaded %d"
          " event(s) instead of being refused", loaded);
    {
        XBOX_INPUT_STATE got;
        memset(&got, 0, sizeof got);
        xbox_PadScriptApplyAtFrame(&got, 100);
        CHECK(got.Gamepad.wButtons == 0 &&
              got.Gamepad.bAnalogButtons[0] == 0,
              "a refused recording still pressed something");
    }

    /* ── 5. ... unless the operator insists, loudly ─────────────────── */
    xbox_PadScriptReset();
    setenv("RECOMP_PAD_REPLAY_FORCE", "1", 1);
    loaded = xbox_PadScriptLoad(spec);
    CHECK(loaded > 0, "RECOMP_PAD_REPLAY_FORCE did not override the refusal");
    unsetenv("RECOMP_PAD_REPLAY_FORCE");

    /* ── 6. a truncated recording is CAUGHT, not replayed as if whole ─ */
    {
        char cut[512];
        FILE *in, *out;
        char buf[1024];
        unsigned long events = 0;
        snprintf(cut, sizeof cut, "%s.cut", path);
        in = fopen(path, "r");
        out = fopen(cut, "w");
        CHECK(in && out, "could not build the truncated case");
        if (in && out) {
            while (fgets(buf, sizeof buf, in)) {
                /* Drop one ordinary event from the middle: the checkpoint
                 * that covers it must then disagree. Counted by event rather
                 * than by line number, so the case does not quietly stop
                 * cutting anything when the format gains a header line. */
                if (buf[0] == 'f' && ++events == 50) continue;
                fputs(buf, out);
            }
        }
        if (in) fclose(in);
        if (out) fclose(out);
        xbox_PadScriptReset();
        xbox_PadRecordSetIdentity("test-build", "test-gen");
        snprintf(buf, sizeof buf, "@%s", cut);
        xbox_PadScriptLoad(buf);
        for (f = 0; f <= FRAMES; f++) {
            XBOX_INPUT_STATE got;
            memset(&got, 0, sizeof got);
            g_anchor_frame = f;
            xbox_PadScriptApplyAtFrame(&got, f);
        }
        ck_bad = 0;
        xbox_PadReplayStatus(&ck_ok, &ck_bad, &at, &state_bad);
        CHECK(ck_bad > 0, "a recording with one event cut out of the middle"
              " replayed with every checkpoint matching -- the divergence"
              " check is not checking anything");
        remove(cut);
    }

    /* ── 7. a state anchor that has slid is reported ────────────────── */
    {
        char b[600];
        xbox_PadScriptReset();
        snprintf(b, sizeof b, "@%s", path);
        xbox_PadScriptLoad(b);
        for (f = 0; f <= FRAMES; f++) {
            XBOX_INPUT_STATE got;
            memset(&got, 0, sizeof got);
            /* The same input, but the title is 40 seconds behind where it
             * was: the frames line up and the scene does not. */
            g_anchor_frame = f > 2400 ? f - 2400 : 0;
            xbox_PadScriptApplyAtFrame(&got, f);
        }
        state_bad = 0; ck_bad = 0;
        xbox_PadReplayStatus(&ck_ok, &ck_bad, &at, &state_bad);
        CHECK(state_bad > 0, "the replay ran 40 s behind the recorded scene"
              " and the state anchor said nothing");
        CHECK(ck_bad == 0, "a state slide was reported as an INPUT divergence;"
              " they mean different things and must not be conflated");
    }

    /* ── 8. an unwritable path fails loudly and returns false ───────── */
    {
        int ok;
        xbox_PadRecordClose();
        ok = xbox_PadRecordOpen("/this/directory/cannot/exist/x.padrec");
        CHECK(!ok, "opening a recording under an unwritable path reported"
              " success -- this is the exact failure that cost three days"
              " and 91 silent fopen calls with the icall database");
    }

    /* ── 9. what one sample costs ───────────────────────────────────── */
    {
        char cheap[512];
        const unsigned long N = 200000;
        clock_t t0, t1;
        double per_us;
        unsigned long i;
        snprintf(cheap, sizeof cheap, "%s.cost", path);
        xbox_PadRecordOpen(cheap);
        t0 = clock();
        for (i = 0; i < N; i++)
            xbox_PadRecordSampleAtFrame(&seq[i % FRAMES], i / 2u);
        t1 = clock();
        xbox_PadRecordClose();
        per_us = (double)(t1 - t0) / CLOCKS_PER_SEC * 1e6 / (double)N;
        printf("  one recorded sample costs %.3f us "
               "(%lu samples, changing state on most of them)\n", per_us, N);
        /* A live run polls the pad at ~120 Hz, so this is the per-second cost
         * of the instrument. The bound is deliberately loose -- it exists to
         * catch a catastrophic regression such as an fflush per sample, which
         * would be three orders of magnitude worse, not to police jitter on a
         * loaded build machine. */
        CHECK(per_us < 50.0, "a sample costs %.3f us; at 120 polls/s that is"
              " %.2f ms/s of instrument, which is no longer free", per_us,
              per_us * 120.0 / 1000.0);
        remove(cheap);
    }

    remove(path);
    free(seq);
    free(want);

    if (failures) {
        fprintf(stderr, "pad_record_test: %d failure(s)\n", failures);
        return 1;
    }
    printf("pad_record_test: record -> replay round trip exact over %lu frames,"
           " refusal and divergence both fire\n", FRAMES);
    /* NOT COVERED HERE, and it needs a run: that the guest's frame counter
     * advances once per guest frame in a live session. This test drives the
     * frame explicitly, which is the right way to test a FORMAT and says
     * nothing about the counter. The evidence for the counter is a session
     * log: [PUSHER] flips= must climb by roughly the frame rate while
     * [PAD-POLL] polls= climbs with the clock. */
    return 0;
}
