/*
 * The three kernel exports JSRF imports that nothing implemented.
 *
 * A boot run proves they RESOLVE -- the thunk table went from 116/120 to
 * 118/120 -- but JSRF does not call any of them during boot, so the run is an
 * absence measurement with no positive control behind it. This calls them
 * directly, which is the control.
 *
 * Ordinals 8 (DbgPrint), 91 (IoDismountVolumeByName) and 144
 * (KeSetDisableBoostThread).
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "kernel.h"

static int fail(const char *what)
{
    fprintf(stderr, "FAIL: %s\n", what);
    return 1;
}

/* ── ordinal 8 ───────────────────────────────────────────── */
static int test_dbgprint(void)
{
    ULONG n;

    /* Returns the character count, which is what a caller checking for
     * truncation reads. "abc 42" is six characters. */
    n = xbox_DbgPrint("abc %d", 42);
    if (n != 6)
        return fail("DbgPrint returned the wrong character count");

    /* A NULL format is a caller error, not a crash. */
    if (xbox_DbgPrint(NULL) != 0)
        return fail("DbgPrint(NULL) should return 0");

    /* The conversions a title actually uses. Nothing to assert beyond not
     * faulting and counting correctly: the text goes to stderr. */
    n = xbox_DbgPrint("%s/%x/%c", "str", 0xABu, 'Z');
    if (n != strlen("str/ab/Z"))
        return fail("DbgPrint mis-counted a mixed conversion");

    return 0;
}

/* ── ordinal 91 ──────────────────────────────────────────── */
static int test_dismount(void)
{
    XBOX_ANSI_STRING name;
    char buf[] = "\\??\\D:";

    if (xbox_IoDismountVolumeByName(NULL) != STATUS_INVALID_PARAMETER)
        return fail("IoDismountVolumeByName(NULL) should be INVALID_PARAMETER");

    /* An empty name is also a caller error -- it names no device. */
    name.Length = 0;
    name.MaximumLength = 0;
    name.Buffer = buf;
    if (xbox_IoDismountVolumeByName(&name) != STATUS_INVALID_PARAMETER)
        return fail("an empty volume name should be INVALID_PARAMETER");

    /* A real name succeeds: there is no mount to tear down, so the
     * postcondition the caller wants already holds. */
    name.Length = (USHORT)strlen(buf);
    name.MaximumLength = (USHORT)(name.Length + 1);
    name.Buffer = buf;
    if (xbox_IoDismountVolumeByName(&name) != STATUS_SUCCESS)
        return fail("dismounting a named volume should succeed");

    return 0;
}

/* ── ordinal 144 ─────────────────────────────────────────── */
static int test_disable_boost(void)
{
    HANDLE self = GetCurrentThread();
    BOOLEAN first, second, third;

    /* The contract that matters is save-set-restore: the value returned is the
     * PREVIOUS one. A version that always answered FALSE would pass a single
     * call and corrupt every restore, so the sequence is what is checked. */
    first  = xbox_KeSetDisableBoostThread(self, TRUE);
    second = xbox_KeSetDisableBoostThread(self, TRUE);
    if (!second)
        return fail("after disabling, the previous state should read TRUE");

    third = xbox_KeSetDisableBoostThread(self, first);
    if (!third)
        return fail("the state set by the second call did not persist");

    /* Restored to whatever it was on entry. */
    if (xbox_KeSetDisableBoostThread(self, first) != first)
        return fail("restore did not put the flag back");

    return 0;
}

int main(void)
{
    if (test_dbgprint())      return 1;
    if (test_dismount())      return 1;
    if (test_disable_boost()) return 1;

    printf("ok  ordinals 8, 91 and 144 behave as their callers expect\n");
    return 0;
}
