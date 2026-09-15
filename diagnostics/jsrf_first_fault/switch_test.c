/* The rule that three A/Bs were decided without.
 *
 * recomp_switch_on() exists because `getenv(X) != NULL` reads =0 as ON, which
 * silently ran RECOMP_APU_SELFLINK_END and RECOMP_VSH_REUSE with the feature
 * enabled in BOTH arms of an A/B and produced numbers that were believed. The
 * predicate is four lines, so it is exactly the kind of thing nobody tests --
 * and the defect it replaces was also four lines.
 *
 * The cases that matter are the two that changed meaning (=0 and empty) and
 * the ones that must NOT have changed, because the conversion is only safe if
 * everything that turned a switch on before still does.
 */
#include "../../src/recomp_switch.h"
#include <stdio.h>

static int fails;
static void expect(const char *value, int want)
{
    int got;
    if (value) setenv("RECOMP_SWITCH_TEST", value, 1);
    else       unsetenv("RECOMP_SWITCH_TEST");
    got = recomp_switch_on("RECOMP_SWITCH_TEST");
    if (got != want) {
        fprintf(stderr, "RECOMP_SWITCH_TEST=%-8s -> %d, wanted %d\n",
                value ? value : "<unset>", got, want);
        ++fails;
    }
}

int main(void)
{
    /* The two spellings that were inverted before. */
    expect("0", 0);
    expect("",  0);          /* `VAR= cmd` is how a shell unsets for one command */
    expect(NULL, 0);

    /* Everything that turned a switch ON before must still turn it on, or the
     * conversion is a silent behaviour change in every script in the tree. */
    expect("1", 1);
    expect("2", 1);
    expect("yes", 1);
    expect("always", 1);
    expect("00", 1);         /* not the literal "0"; deliberately still on */
    expect("0x1", 1);

    /* An unset variable must not be confused with one set to a falsey value:
     * both are off, but a NEARBY variable must be unaffected. Cheap guard
     * against a predicate that caches or reads the wrong name. */
    setenv("RECOMP_SWITCH_TEST_OTHER", "1", 1);
    expect("0", 0);
    if (!recomp_switch_on("RECOMP_SWITCH_TEST_OTHER")) {
        fprintf(stderr, "a neighbouring variable was misread\n");
        ++fails;
    }

    if (fails) { fprintf(stderr, "%d case(s) wrong\n", fails); return 1; }
    printf("recomp_switch_on: =0 and empty are OFF, every other value is ON\n");
    return 0;
}
