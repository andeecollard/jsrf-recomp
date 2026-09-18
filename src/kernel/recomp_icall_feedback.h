/*
 * Indirect-branch target feedback - shared constants.
 *
 * Included by both src/kernel/icall_feedback.c (which defines the array) and
 * templates/runtime/recomp_types.h (which emits the store into the ICALL
 * macros). Kept separate from recomp_types.h so the generated-code header stays
 * a template and this stays a runtime detail.
 *
 * See src/kernel/icall_feedback.c for the rationale and
 * docs/technical/ms-fusion-codegen-teardown.md for where the idea comes from.
 */

#ifndef RECOMP_ICALL_FEEDBACK_H
#define RECOMP_ICALL_FEEDBACK_H

#include <stdint.h>

/* Window covered by the observation array.
 *
 * CUSTOMIZE: must span every executable section of your XBE. The default covers
 * the whole image for a 7.7 MiB XBE (tools/disasm/config.py XBE_BASE_ADDRESS /
 * XBE_IMAGE_SIZE). Targets outside the window are ignored rather than clamped --
 * a wrong bucket is worse than a missing one, because it would seed function
 * detection at an address the title never actually branched to. */
#define RECOMP_ICALL_FB_BASE 0x00010000u
#define RECOMP_ICALL_FB_SIZE 0x00800000u  /* 8 MiB */

/* Flags OR'd into g_icall_seen[va - base]. */
#define RECOMP_ICALL_SEEN_RESOLVED   1u  /* dispatch found a translation */
#define RECOMP_ICALL_SEEN_UNRESOLVED 2u  /* dispatch failed: a real gap */

#ifdef RECOMP_ICALL_FEEDBACK

extern volatile unsigned char g_icall_seen[RECOMP_ICALL_FB_SIZE];

/**
 * Record an observed indirect-branch target.
 *
 * Inlined into the ICALL macros rather than being a function call, because it
 * sits on the hottest path in the generated code: one subtract, one compare,
 * one OR.
 */
#define RECOMP_ICALL_OBSERVE(va, flags) do { \
    uint32_t _obs_off = (uint32_t)(va) - RECOMP_ICALL_FB_BASE; \
    if (_obs_off < RECOMP_ICALL_FB_SIZE) \
        g_icall_seen[_obs_off] |= (unsigned char)(flags); \
} while (0)

/**
 * Write the observed target set to a text file.
 * Merge it into the persisted database with tools/recomp/icall_feedback.py.
 * Safe to call more than once; call it from atexit() and from your crash
 * handler, since a title that dies mid-boot is exactly the one whose targets
 * you want.
 */
void recomp_icall_feedback_dump(const char *path);

/** Register an atexit() dump to recomp_icall_feedback_path(). */
void recomp_icall_feedback_init(void);

/* Default dump location, relative to the working directory. CUSTOMIZE if your
 * launcher runs from somewhere you would rather not write to.
 *
 * A COMPILE-TIME DEFAULT IS NOT ENOUGH, and this cost three days of feedback to
 * learn. A double-clicked macOS .app inherits no shell and starts with a
 * working directory it may not write to, so the relative default fails at
 * fopen -- on the ONE configuration that matters, because a player's session
 * reaches code no scripted run does. It failed 91 times in a single JSRF
 * session, once per periodic report, while the persisted database sat three
 * days stale and nobody noticed: the message named the file but not the
 * variable, the cause, or the fact that nothing was being recorded.
 *
 * So the path is resolvable at RUNTIME from the environment variable of the
 * same name, which is what a launcher can actually set. recomp_icall_feedback_path()
 * does the resolving; the env var wins when set and non-empty. */
#ifndef RECOMP_ICALL_FEEDBACK_PATH
#define RECOMP_ICALL_FEEDBACK_PATH "icall_targets.dump"
#endif

/**
 * The dump path actually in effect: $RECOMP_ICALL_FEEDBACK_PATH if it is set
 * and non-empty, otherwise the compile-time RECOMP_ICALL_FEEDBACK_PATH.
 *
 * Resolved once and cached, so the answer cannot change between the periodic
 * dump, the crash handler and atexit -- three callers that must not disagree
 * about where the run's evidence went. Never returns NULL.
 */
const char *recomp_icall_feedback_path(void);

/* Call these from the host program. They are macros so a host can call them
 * unconditionally without #ifdef -- both compile away when the feature is off,
 * which is the point: instrumentation the host has to bracket in #ifdef is
 * instrumentation that rots. */
#define RECOMP_ICALL_FEEDBACK_INIT() recomp_icall_feedback_init()
#define RECOMP_ICALL_FEEDBACK_DUMP() \
    recomp_icall_feedback_dump(recomp_icall_feedback_path())

#else  /* !RECOMP_ICALL_FEEDBACK */

#define RECOMP_ICALL_OBSERVE(va, flags) ((void)0)
#define RECOMP_ICALL_FEEDBACK_INIT()   ((void)0)
#define RECOMP_ICALL_FEEDBACK_DUMP()   ((void)0)

#endif

#endif /* RECOMP_ICALL_FEEDBACK_H */
