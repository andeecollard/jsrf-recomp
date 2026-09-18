#ifndef RECOMP_SWITCH_H
#define RECOMP_SWITCH_H
#include <stdlib.h>
#include <string.h>

/* IS THIS OPT-IN SWITCH ON? Ask here, never `getenv(name) != NULL`.
 *
 * A presence test is right for a trace -- RECOMP_VOICE_TRACE either prints or
 * it does not, and nobody writes =0 to silence it. It is WRONG for anything
 * that changes what the program does, because the obvious way to take a
 * control arm is to pass the variable as 0, and presence reads that as ON.
 *
 * This has cost three wrong conclusions here, each found only after the
 * numbers had been believed:
 *
 *   RECOMP_APU_SELFLINK_END   an A/B whose control arm ran with the guard on,
 *                             reporting "(guard on)" in both arms
 *   RECOMP_VSH_REUSE          the same, three more predicates, found AFTER the
 *                             first was fixed because nobody swept for others
 *   RECOMP_PB_EXEC            never miscompared, but four presence tests on the
 *                             master switch for the whole NV2A executor, which
 *                             CLAUDE.md's own run line writes as =1
 *
 * xbox_memory_layout.c's RECOMP_OHCI_ATTACH got this right on its own and is
 * where the rule below comes from. It is the conservative conversion: every
 * value that turns a switch on today still turns it on, so nothing that reads
 * "=1" or "=yes" changes meaning, and only the literal "0" -- the one spelling
 * that was silently inverted -- starts meaning off. Empty counts as off too,
 * because `VAR= cmd` is how a shell unsets a variable for one command.
 *
 * A switch whose VALUE carries meaning does not belong here: RECOMP_FAKE_PAD
 * takes 1, <n>, `a` and `always`, so its presence test is correct and atoi()
 * would silently disable `=a`. Read the value directly in that case.
 *
 * ALSO PRINT THE STATE. ab_score.py refuses to compare two arms that report the
 * same switch state, but it can only check a switch that names itself in a
 * report. That is one fprintf and it is the difference between "I set the
 * variable" and "the model read it". */
static inline int recomp_switch_on(const char *name)
{
    const char *v = getenv(name);
    return v && v[0] != '\0' && strcmp(v, "0") != 0;
}

/* THE SAME GRAMMAR, FOR A SWITCH THAT DEFAULTS ON.
 *
 * recomp_switch_on cannot express this: unset reads as off, which is the one
 * answer a default-on switch must not give. Every default-on switch in this
 * tree has therefore hand-rolled its own getenv, and each one re-decided the
 * grammar -- RECOMP_APU_FEDEC_HOLD read an exported-but-empty value as OFF for
 * a week because it tested `e` rather than `e && *e`.
 *
 * So: unset or empty means the default; anything else goes through the same
 * rule as recomp_switch_on, so "0" is off and "on", "yes" and "false" are all
 * ON exactly as they are everywhere else in this tree. Disagreeing with that
 * is what the switch ratchet exists to prevent. */
static inline int recomp_switch_on_default(const char *name, int dflt)
{
    const char *v = getenv(name);
    if (!v || v[0] == '\0') return dflt != 0;
    return strcmp(v, "0") != 0;
}

#endif /* RECOMP_SWITCH_H */
