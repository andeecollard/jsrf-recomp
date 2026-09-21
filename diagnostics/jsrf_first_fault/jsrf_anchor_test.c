/*
 * The state anchor's misalignment report must NAME the field that moved.
 *
 * The replay's scene check has been able to fail since 20 Sep 2026, when
 * CActSequence::m_dwNextMethod went into the anchor's top byte and gave the
 * comparison something that varies. What it could not do was say WHAT varied:
 * the line read "was at guest state 1E000003, this run is at 1F000003" and
 * left a bitfield to be decoded by hand, by someone already mid-investigation.
 *
 * Four fields share one 32-bit word because that is the recording format.
 * These checks pin the decode to that packing. They link the real describer
 * out of jsrf_anchor.c -- a copy here could agree with a mislabelling bug.
 *
 * The 'minutes' case matters most: it is the only field the input layer
 * compares with slack, so a report that did not mark it slow would invite
 * reading a one-minute gap as a real divergence.
 */
#include <stdio.h>
#include <string.h>

#include "jsrf_anchor.h"

static int failures;

static void expect(const char *what, unsigned long rec, unsigned long live,
                   const char *want)
{
    char buf[192];
    jsrf_pad_anchor_describe(rec, live, buf, sizeof buf);
    if (strstr(buf, want)) {
        printf("  ok   %-32s \"%s\"\n", what, buf);
        return;
    }
    printf("  FAIL %-32s got \"%s\", want substring \"%s\"\n", what, buf, want);
    ++failures;
}

int main(void)
{
    char buf[192];

    /* Layout: sequence<<24 | chapter<<20 | mission<<16 | minutes */
    expect("sequence moved", 0x1E000003ul, 0x1F000003ul, "sequence 30->31");
    expect("chapter moved",  0x1E100003ul, 0x1E200003ul, "chapter 1->2");
    expect("mission moved",  0x1E010003ul, 0x1E020003ul, "mission 1->2");
    expect("the slow counter is marked",
           0x1E000003ul, 0x1E000009ul, "minutes 3->9 (slow counter)");
    expect("two fields at once",
           0x1E000003ul, 0x1F000009ul, "sequence 30->31, minutes 3->9");

    /* Agreement must say nothing at all: the caller appends this phrase to a
     * line it only prints on disagreement, and "differs in: " followed by
     * nothing reads as an instrument fault. */
    jsrf_pad_anchor_describe(0x1E000003ul, 0x1E000003ul, buf, sizeof buf);
    if (buf[0]) {
        printf("  FAIL identical anchors described \"%s\"\n", buf);
        ++failures;
    } else {
        printf("  ok   identical anchors say nothing\n");
    }

    /* A recording from a build that packed the word differently can disagree
     * in bits no field claims. Saying so beats an empty phrase. */
    jsrf_pad_anchor_describe(0ul, 0x00008000ul, buf, sizeof buf);
    if (!strstr(buf, "minutes") && !strstr(buf, "no named field")) {
        printf("  FAIL unclaimed bits described \"%s\"\n", buf);
        ++failures;
    } else {
        printf("  ok   unclaimed difference is still reported\n");
    }

    /* It formats into a fixed caller buffer; truncation must not run off it. */
    {
        char small[8];
        jsrf_pad_anchor_describe(0x1E000003ul, 0x1F000009ul, small, sizeof small);
        if (small[sizeof small - 1]) {
            printf("  FAIL short buffer left unterminated\n");
            ++failures;
        } else {
            printf("  ok   truncates safely into a short buffer\n");
        }
    }

    printf("jsrf_anchor_test: %s\n", failures ? "FAIL" : "pass");
    return failures ? 1 : 0;
}
