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
#include "jsrf_anchor.h"

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
/* Every recording taken before 20 Sep 2026 carries this at every
 * checkpoint: the fields the anchor read stayed zero until a save was
 * written, so the reference side could never disagree. */
static unsigned long test_anchor_constant(void) { return 0ul; }

/* Two anchors in JSRF's own packing that differ in EXACTLY ONE named field:
 * sequence<<24 | chapter<<20 | mission<<16 | minutes, sequence 30 vs 31.
 *
 * The minute counter has to MOVE in both. A recording whose anchors never
 * vary is UNVERIFIED by design -- section 8 is that rule -- so a constant
 * reference here would be refused before any comparison happened, and the
 * field-naming check would pass by never running. It moves identically on
 * both sides, so the only difference the comparison can see is the one this
 * section is about. */
static unsigned long test_anchor_seq30(void)
{ return 0x1E120000ul | ((g_anchor_frame / 60u) & 0xFFFFul); }
static unsigned long test_anchor_seq31(void)
{ return 0x1F120000ul | ((g_anchor_frame / 60u) & 0xFFFFul); }

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

    /* ── 7. a mark round-trips, and does not touch the input hash ───── */
    {
        char marked[128];
        const char *lab = NULL;
        unsigned long long h_plain, h_marked;
        int n;
        snprintf(marked, sizeof marked, "/tmp/jsrf_padrec_mark_%d.padrec", (int)getpid());
        /* Record the same sequence twice, once with a mark in the middle.
         * The mark must come back from the file with its frame and label,
         * must answer a near-mark query within slack and not outside it,
         * and must leave the pad hash IDENTICAL: a mark is not input. */
        xbox_PadRecordSetIdentity("test-build", "test-gen");
        record_sequence(path, seq, FRAMES);
        h_plain = xbox_PadRecordHash();
        CHECK(xbox_PadRecordOpen(marked), "could not open %s", marked);
        for (f = 0; f < FRAMES; f++) {
            unsigned p, np = polls_for_frame(f);
            g_anchor_frame = f;
            if (f == 100) {
                /* the mark is stamped with the guest frame counter, which
                 * this test does not drive; so drive it to 100 for the
                 * press and back to where the recorder expects it. */
                while (xbox_InputFrame() < 100) xbox_InputFrameAdvance();
                xbox_PadRecordMark("text wrong here");
            }
            for (p = 0; p < np; p++)
                xbox_PadRecordSampleAtFrame(&seq[f], f);
        }
        xbox_PadRecordClose();
        h_marked = xbox_PadRecordHash();
        CHECK(h_marked == h_plain, "a mark changed the pad hash (%016llx vs"
              " %016llx); a marked recording would diverge on replay",
              (unsigned long long)h_marked, (unsigned long long)h_plain);

        xbox_PadScriptReset();
        CHECK(!xbox_PadNearMark(100, 0, &lab), "a mark survived reset");
        snprintf(spec, sizeof spec, "@%s", marked);
        n = xbox_PadScriptLoad(spec);
        CHECK(n > 0, "the marked recording did not load (%d)", n);
        CHECK(xbox_PadNearMark(100, 0, &lab) && lab && !strcmp(lab, "text wrong here"),
              "the mark did not come back from the file (label %s)", lab ? lab : "(null)");
        CHECK(xbox_PadNearMark(130, 30, &lab), "a frame 30 away with slack 30 is not near");
        CHECK(!xbox_PadNearMark(131, 30, &lab), "a frame 31 away with slack 30 is near");
        /* and the replay of it is still exact: the directive is not an event */
        mismatches = 0;
        for (f = 0; f < FRAMES; f++) {
            XBOX_INPUT_STATE got;
            memset(&got, 0, sizeof got);
            g_anchor_frame = f;
            xbox_PadScriptApplyAtFrame(&got, f);
            if (!gamepad_eq(&got.Gamepad, &want[f].Gamepad)) { mismatches++; }
        }
        CHECK(mismatches == 0, "the marked recording replays differently");
        remove(marked);
    }

    /* ── 10. a recording whose anchor never varies stays UNVERIFIED ──
     *
     * The scene half of the verdict is only meaningful when the reference side
     * can disagree. Recorded with a constant anchor and replayed against one
     * that moves, this must NOT be reported as a state misalignment: the
     * recording predates the anchor carrying anything, which is a different
     * fact from the title being in the wrong place, and conflating them would
     * turn every old recording into a false DIVERGED. */
    {
        char b[600];
        char cpath[600];
        int ck_ok2 = 0, ck_bad2 = 0, state_bad2 = 0;
        unsigned long at2 = 0;

        snprintf(cpath, sizeof cpath, "%s.constanchor", path);
        xbox_PadRecordSetAnchorFn(test_anchor_constant);
        xbox_PadRecordClose();
        xbox_PadRecordOpen(cpath);
        for (f = 0; f <= FRAMES; f++) {
            g_anchor_frame = f;
            xbox_PadRecordSampleAtFrame(&seq[f % FRAMES], f);
        }
        xbox_PadRecordClose();

        /* Replay it with an anchor that does move and disagrees throughout. */
        xbox_PadRecordSetAnchorFn(test_anchor);
        xbox_PadScriptReset();
        snprintf(b, sizeof b, "@%s", cpath);
        xbox_PadScriptLoad(b);
        for (f = 0; f <= FRAMES; f++) {
            XBOX_INPUT_STATE got;
            memset(&got, 0, sizeof got);
            g_anchor_frame = f;
            xbox_PadScriptApplyAtFrame(&got, f);
        }
        xbox_PadReplayStatus(&ck_ok2, &ck_bad2, &at2, &state_bad2);
        CHECK(state_bad2 == 0, "a recording with a constant anchor reported %d"
              " state misalignment(s); a reference that cannot disagree must"
              " not be scored as a disagreement", state_bad2);
        CHECK(ck_bad2 == 0, "the input round trip broke while checking the"
              " constant-anchor case, so its verdict means nothing");
        remove(cpath);
        xbox_PadRecordSetAnchorFn(test_anchor);
    }

    /* ── 9. the replay's misalignment report NAMES the field ────────────
     *
     * Sections 7 and 8 prove the anchor comparison can fail and that a
     * constant anchor is not scored as a failure. Neither exercises what a
     * human then reads. Until now that was two hex words, and the describer
     * added to name the field was covered only by its own unit test -- the
     * callback had never fired through the actual record-and-replay path.
     *
     * So: record with one anchor, replay with another differing in exactly
     * one field, and read back the line the input layer emitted. stderr is
     * redirected around the replay because the emitted TEXT is the thing
     * under test; asserting on the callback's arguments instead would pass
     * even if the layer never printed what it was given. */
    {
        char mpath[600], lpath[600], line[4096];
        int ck_ok3 = 0, ck_bad3 = 0, state_bad3 = 0, named = 0, saw_misalign = 0;
        unsigned long at3 = 0;
        size_t n;
        FILE *cap;

        snprintf(mpath, sizeof mpath, "%s.named", path);
        snprintf(lpath, sizeof lpath, "%s.namedlog", path);

        xbox_PadRecordSetAnchorFn(test_anchor_seq30);
        xbox_PadRecordSetAnchorDescribeFn(jsrf_pad_anchor_describe);
        xbox_PadRecordClose();
        xbox_PadRecordOpen(mpath);
        for (f = 0; f <= FRAMES; f++) {
            g_anchor_frame = f;
            xbox_PadRecordSampleAtFrame(&seq[f % FRAMES], f);
        }
        xbox_PadRecordClose();

        /* The controlled mismatch: same recording, sequence one higher. */
        xbox_PadRecordSetAnchorFn(test_anchor_seq31);
        xbox_PadScriptReset();
        snprintf(line, sizeof line, "@%s", mpath);
        xbox_PadScriptLoad(line);

        /* dup2, not freopen. Restoring with freopen("/dev/tty") loses stderr
         * outright when there is no tty -- which is every ctest run, and is
         * how this check first "passed" by printing nothing at all. Saving
         * the descriptor and putting it back has no such dependency. */
        fflush(stderr);
        {
            int saved = dup(fileno(stderr));
            FILE *to = fopen(lpath, "w");
            if (to) {
                dup2(fileno(to), fileno(stderr));
                for (f = 0; f <= FRAMES; f++) {
                    XBOX_INPUT_STATE got;
                    memset(&got, 0, sizeof got);
                    g_anchor_frame = f;
                    xbox_PadScriptApplyAtFrame(&got, f);
                }
                fflush(stderr);
                fclose(to);
            }
            if (saved >= 0) {
                dup2(saved, fileno(stderr));
                close(saved);
            }
        }
        xbox_PadReplayStatus(&ck_ok3, &ck_bad3, &at3, &state_bad3);

        cap = fopen(lpath, "r");
        if (cap) {
            while (fgets(line, sizeof line, cap)) {
                if (strstr(line, "STATE MISALIGNED")) {
                    saw_misalign = 1;
                    if (strstr(line, "Differs in: sequence 30->31"))
                        named = 1;
                    /* The fields that did NOT move must not be mentioned. */
                    if (strstr(line, "chapter") || strstr(line, "mission")
                            || strstr(line, "minutes"))
                        named = -1;
                }
            }
            fclose(cap);
        }
        remove(lpath);
        remove(mpath);

        CHECK(state_bad3 > 0, "a one-field anchor mismatch was not reported"
              " as a state misalignment at all");
        CHECK(saw_misalign, "no STATE MISALIGNED line was emitted for a"
              " mismatch the status call counted");
        CHECK(named == 1, "the misalignment line did not name the changed"
              " field as \"sequence 30->31\" (named=%d); a report that names"
              " the wrong field is worse than the hex it replaced", named);

        xbox_PadRecordSetAnchorDescribeFn(NULL);
        xbox_PadRecordSetAnchorFn(test_anchor);
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
