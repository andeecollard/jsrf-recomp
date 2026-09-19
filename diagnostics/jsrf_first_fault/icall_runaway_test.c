/* CAN THE RUNTIME TELL A SPIN FROM A HICCUP, AND DOES IT SAY SO ONCE?
 *
 * A skipped indirect call is not by itself a defect. Scored across the
 * 782-run corpus on 19 Sep 2026, 20 runs carried between one and ten of them
 * and finished normally, while 17 runs went into a spin -- the same target,
 * always 0x00000000, called until the process was killed: 10,863,700,000 of
 * them in one run and 23,429,500,000 in another. The old rate limit printed
 * every ten-thousandth, so those two runs produced 1.09M and 2.34M lines of
 * one sentence, and the sentence did not name the cause.
 *
 * The cause is always upstream and always in the log already, tens of
 * thousands of lines earlier: an indirect call whose VA the dispatch table
 * could not resolve at all. It is skipped, the caller resumes as if the
 * callee had run and returned, and the registers the callee was supposed to
 * restore are whatever the skipped body left behind. The wreck loops.
 *
 * Three properties, and each has failed in a way that reads like working:
 *
 *   1. The cadence is bounded. "Every ten thousand" is not a rate limit; it
 *      is a slower flood. Powers of ten cap one address at about twenty lines
 *      for any count a 64-bit counter can hold.
 *   2. Nothing a healthy run does changes. The two cadences agree exactly up
 *      to 10,000 hits on one address and first differ at 20,000; the corpus
 *      puts a harmless trickle at one to ten calls, so the whole of normal is
 *      inside the band where nothing moved. Both halves are asserted, because
 *      "nothing changes" on its own would be a slightly false claim and a
 *      slightly false claim is how a regression gets waved through.
 *   3. The threshold and the abort are read from the environment, and the
 *      abort is OFF unless asked for. Naming a runaway costs a good run
 *      nothing; ending one is a behaviour change and has to be requested.
 *
 * Property 3's accessors cache their getenv in a static, so each is driven in
 * its own process by ctest, the way jsrf_icall_feedback_path_test is.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stddef.h>

extern int recomp_icall_log_cadence(uint64_t hits);
extern uint64_t recomp_icall_runaway_threshold(void);
extern int recomp_icall_runaway_aborts(void);
extern uint32_t recomp_itail_frame_top(uint32_t sp, uint32_t limit,
                                       uint32_t *loose_out);

/* The guest memory window the scan reads through, and the code range it
 * judges an address against. Writing them is how this test gives the scanner
 * a stack to walk without a game. */
extern ptrdiff_t g_xbox_mem_offset;
extern uint32_t g_xbox_code_lo;
extern uint32_t g_xbox_code_hi;

static int fails;

static void check(int ok, const char *what)
{
    if (!ok) {
        printf("FAIL %s\n", what);
        fails++;
    }
}

int main(void)
{
    const char *env_limit = getenv("RECOMP_ICALL_RUNAWAY");
    const char *env_abort = getenv("RECOMP_ICALL_RUNAWAY_ABORT");
    uint64_t h;
    int lines;

    /* 1. The cadence the old code had, unchanged, up to where it ended. */
    check(recomp_icall_log_cadence(1) == 1, "first skip must print");
    check(recomp_icall_log_cadence(10) == 1, "tenth skip must print");
    check(recomp_icall_log_cadence(100) == 1, "hundredth skip must print");
    check(recomp_icall_log_cadence(1000) == 1, "thousandth skip must print");
    check(recomp_icall_log_cadence(10000) == 1, "ten-thousandth skip must print");

    /* 2. And silence in between -- this is the half that makes it a limit. */
    check(recomp_icall_log_cadence(0) == 0, "a zero count is not a hit");
    check(recomp_icall_log_cadence(2) == 0, "second skip must be silent");
    check(recomp_icall_log_cadence(9) == 0, "ninth skip must be silent");
    check(recomp_icall_log_cadence(999) == 0, "999th skip must be silent");
    check(recomp_icall_log_cadence(9999) == 0, "9999th skip must be silent");
    check(recomp_icall_log_cadence(10001) == 0, "10001st skip must be silent");

    /* Every count a healthy run can reach behaves exactly as it did before
     * the change. The old rule was 1, 10, 100, 1000 and then every 10,000, so
     * the two agree on every value up to and including 10,000 -- checked
     * exhaustively rather than at the corners, because the corners are where
     * a rewritten predicate looks right. The corpus puts a harmless trickle
     * at one to ten calls, so this band is the whole of normal. */
    for (h = 1; h <= 10000; h++) {
        int was = (h == 1 || h == 10 || h == 100 || h == 1000
                   || (h % 10000) == 0);
        if (recomp_icall_log_cadence(h) != was) {
            printf("FAIL cadence changed at %llu: was %d now %d\n",
                   (unsigned long long)h, was, recomp_icall_log_cadence(h));
            fails++;
            break;
        }
    }

    /* And 20,000 is exactly where they part company, deliberately. Pin it,
     * because "nothing changes" was the claim this test was written to make
     * and it is not quite true: past ten thousand hits on ONE address the old
     * rule keeps printing for ever and the new one stops. Saying which count
     * is the first to differ is the honest version of that claim. */
    check(recomp_icall_log_cadence(20000) == 0,
          "20000 is the first count the new cadence drops");
    check(recomp_icall_log_cadence(100000) == 1,
          "the next decade still prints");

    /* 3. Bounded above. The run that reached 23,429,500,000 skips printed
     *    2.34M lines; the same run must now print twenty. */
    lines = 0;
    for (h = 1; h <= 1000000000000000000ULL; h *= 10) {
        if (recomp_icall_log_cadence(h)) lines++;
    }
    check(lines <= 24, "a decade ladder must stay under two dozen lines");
    check(recomp_icall_log_cadence(23429500000ULL) == 0,
          "a count that is not a power of ten must not print");
    check(recomp_icall_log_cadence(10000000000ULL) == 1,
          "ten billion is a power of ten and must print");

    /* 4. The threshold. Unset it is a million -- the line the corpus analysis
     *    drew between the 17 dead runs and the 20 that carried a trickle. */
    if (!env_limit) {
        check(recomp_icall_runaway_threshold() == 1000000ULL,
              "default threshold must be 1e6");
    } else if (!*env_limit) {
        check(recomp_icall_runaway_threshold() == 1000000ULL,
              "an empty variable must not override the default");
    } else {
        unsigned long long want = strtoull(env_limit, NULL, 0);
        check(recomp_icall_runaway_threshold() == (uint64_t)want,
              "threshold must come from RECOMP_ICALL_RUNAWAY");
    }
    /* Cached: the check runs on every skipped call and must not change its
     * mind halfway through a run. */
    check(recomp_icall_runaway_threshold() == recomp_icall_runaway_threshold(),
          "threshold must be stable across calls");

    /* 5. The abort. Off unless asked, because ending a run is the one thing
     *    here that a healthy run would notice. */
    if (!env_abort || !*env_abort || *env_abort == '0') {
        check(recomp_icall_runaway_aborts() == 0,
              "abort must be off unless asked for");
    } else {
        check(recomp_icall_runaway_aborts() == 1,
              "RECOMP_ICALL_RUNAWAY_ABORT must arm the abort");
    }

    /* 6. THE STRANDED-FRAME SCAN, and the exact shape that fooled it.
     *
     * Pre-registered on 19 Sep 2026: sub_00114A80's frame is 0xA4 bytes
     * (`sub esp,0x98` plus ebx, ebp, esi), confirmed to the byte by its own
     * `mov eax,[esp+0xa8]` reading the stdcall argument at esp_entry+4 and by
     * the `ret 4` it never reaches. The player's run reported 72. The frame
     * arithmetic was right and the SCAN was wrong: it stopped at the first
     * stack slot holding any code address, and a live frame is full of code
     * addresses that are not return addresses.
     *
     * So build that frame. A function pointer at esp+72 pointing at a
     * function START, the real return address at esp+164 pointing just past a
     * `call rel32`, and nothing else. The old scan answers 72. The scan has
     * to answer 164 and offer 72 only as the thing it stepped over. */
    {
        enum { BASE = 0x00400000, SIZE = 0x2000 };
        enum { CODE_LO = BASE, CODE_HI = BASE + 0x1000 };
        enum { STACK = BASE + 0x1800 };
        enum { FUNC_START = BASE + 0x0100 };   /* a callee, not a return site */
        enum { RET_SITE   = BASE + 0x0200 };   /* immediately after a call    */
        unsigned char *mem = calloc(SIZE, 1);
        ptrdiff_t saved_off = g_xbox_mem_offset;
        uint32_t saved_lo = g_xbox_code_lo, saved_hi = g_xbox_code_hi;
        uint32_t loose = 0, top;

        if (!mem) { printf("FAIL out of memory\n"); return 1; }
        g_xbox_mem_offset = (ptrdiff_t)((uintptr_t)mem - (uintptr_t)BASE);
        g_xbox_code_lo = CODE_LO;
        g_xbox_code_hi = CODE_HI;

        /* A function start: `push ebp` preceded by int3 padding. Nothing that
         * could be read as a call ends here. */
        mem[FUNC_START - BASE - 1] = 0xCC;
        mem[FUNC_START - BASE] = 0x55;
        /* A return site: `E8 rel32` ends exactly at RET_SITE. */
        mem[RET_SITE - BASE - 5] = 0xE8;

        /* The frame. */
        *(uint32_t *)(mem + (STACK - BASE) + 0)   = 0x040D3A70;  /* saved esi */
        *(uint32_t *)(mem + (STACK - BASE) + 72)  = FUNC_START;  /* the decoy */
        *(uint32_t *)(mem + (STACK - BASE) + 164) = RET_SITE;    /* the truth */

        top = recomp_itail_frame_top(STACK, 256, &loose);
        check(top == STACK + 164,
              "the scan must step over a function pointer and find the return"
              " address at esp+164");
        check(loose == STACK + 72,
              "and must still report the nearer code pointer as what it"
              " stepped over");
        printf("  frame scan: return site at esp+%u, nearest code pointer at"
               " esp+%u\n",
               top ? (unsigned)(top - STACK) : 0u,
               loose ? (unsigned)(loose - STACK) : 0u);

        /* With no return site at all the answer must be zero -- the caller
         * then reports a FLOOR rather than inventing a frame size. */
        *(uint32_t *)(mem + (STACK - BASE) + 164) = FUNC_START;
        loose = 0;
        top = recomp_itail_frame_top(STACK, 256, &loose);
        check(top == 0, "no return site must report none, not the decoy");
        check(loose == STACK + 72, "and must still hand back the floor");

        g_xbox_mem_offset = saved_off;
        g_xbox_code_lo = saved_lo;
        g_xbox_code_hi = saved_hi;
        free(mem);
    }

    printf("  RECOMP_ICALL_RUNAWAY=%s -> threshold %llu\n",
           env_limit ? (*env_limit ? env_limit : "(empty)") : "(unset)",
           (unsigned long long)recomp_icall_runaway_threshold());
    printf("  RECOMP_ICALL_RUNAWAY_ABORT=%s -> aborts %d\n",
           env_abort ? (*env_abort ? env_abort : "(empty)") : "(unset)",
           recomp_icall_runaway_aborts());

    if (fails) {
        printf("FAILED %d check(s)\n", fails);
        return 1;
    }
    printf("OK\n");
    return 0;
}
