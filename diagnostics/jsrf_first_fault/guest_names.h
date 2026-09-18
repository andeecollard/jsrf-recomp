/* Guest-VA -> recovered name, for crash reports and traces.
 *
 * The recompiled functions are called sub_<GUESTVA>, so every host symbol
 * already carries the guest address that names it. This maps that address to a
 * real name when one is known, without regenerating anything.
 *
 * Names come from Microsoft's own Fission PrecompiledSymbolTable, transferred
 * to this title by size-sequence alignment (see xex_tools/). They are a
 * DIAGNOSTIC AID, not ground truth: coverage is partial and the transfer has a
 * measured error rate. Nothing may branch on them.
 */
#ifndef RECOMP_GUEST_NAMES_H
#define RECOMP_GUEST_NAMES_H
#include <stdint.h>
#include <stddef.h>

/* Load "<hex va>\t<name>" lines from $RECOMP_GUEST_NAMES. Safe to call twice;
 * the second call is a no-op. Returns the number of names loaded. */
size_t recomp_guest_names_load(void);

/* Exact match on a function start, or NULL. Allocation-free and reentrant once
 * loaded, so the crash handler may call it. */
const char *recomp_guest_name(uint32_t va);

/* For the report line and for tests: how many names are loaded. */
size_t recomp_guest_names_count(void);

/* Parse a guest VA out of a recompiled symbol such as "sub_001A2E2E" (with or
 * without a leading underscore). Returns 0 on no match. */
int recomp_guest_va_from_symbol(const char *sym, uint32_t *out);
#endif
