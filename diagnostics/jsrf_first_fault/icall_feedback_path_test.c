/* WHERE DOES THE INDIRECT-BRANCH FEEDBACK ACTUALLY GET WRITTEN?
 *
 * The dump path was a compile-time constant, relative to the working
 * directory. A double-clicked macOS .app has no writable working directory, so
 * every player session since the bundle became the test path wrote nothing --
 * 91 fopen failures in one run, one per periodic report, against a persisted
 * database that sat three days stale. The feedback loop this project adopted
 * from Microsoft's VirtualDispatchTraceFiles was dead on exactly the runs worth
 * feeding it, and the symptom was a line that named the file but not the cause.
 *
 * The path is now resolvable from the environment. This test exists because
 * the failure mode is silence: a resolver that ignores the variable returns
 * the old default, the launcher looks configured, and the dump goes back to
 * nowhere. That reads identically to working.
 *
 * Driven by ctest three times -- unset, set, and set-but-empty -- because
 * recomp_icall_feedback_path() caches its getenv in a static on first use, so
 * one process can only ever observe one answer.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern const char *recomp_icall_feedback_path(void);

#define COMPILED_DEFAULT "icall_targets.dump"

int main(void)
{
    const char *env  = getenv("RECOMP_ICALL_FEEDBACK_PATH");
    const char *want = (env && *env) ? env : COMPILED_DEFAULT;
    const char *got  = recomp_icall_feedback_path();
    const char *again;

    if (!got) {
        printf("FAIL resolver returned NULL\n");
        return 1;
    }
    printf("  env=%s\n", env ? (*env ? env : "(set, empty)") : "(unset)");
    printf("  want=%s\n  got =%s\n", want, got);

    if (strcmp(got, want) != 0) {
        printf("FAIL wrong path\n");
        return 1;
    }

    /* Cached: three callers -- periodic report, crash handler, atexit -- must
     * not disagree about where the run's evidence went. */
    again = recomp_icall_feedback_path();
    if (!again || strcmp(again, got) != 0) {
        printf("FAIL second call disagreed: %s\n", again ? again : "(null)");
        return 1;
    }

    /* An env var that is set but empty must NOT win. fopen("") fails, so
     * honouring it would turn a blank line in a config file into exactly the
     * silent non-recording this whole change exists to remove. */
    if (env && !*env && strcmp(got, COMPILED_DEFAULT) != 0) {
        printf("FAIL empty env overrode the default\n");
        return 1;
    }

    printf("ok\n");
    return 0;
}
