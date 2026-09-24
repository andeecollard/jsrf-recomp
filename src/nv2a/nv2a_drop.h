#ifndef NV2A_DROP_H
#define NV2A_DROP_H
/* G54: EVERY DRAW THE EXECUTOR DROPS, SKIPS OR SIMPLIFIES SAYS SO.
 *
 * Fog showed how a whole class of draws can vanish behind one easily misread
 * counter: 926,734 fogged draws refused in a session whose only trace was
 * "[COMBINER] refusals: final-cw=926734", and then 465,058 more behind a
 * second gate reported as "[TEXTURE] 465058 fog". Every place that drops a
 * draw (DROPPED), draws it approximated (SIMPLIFIED) or loses part of it
 * (PARTIAL, counted in triangles) calls nv2a_drop with a stage, a reason (a
 * string literal) and a detail word. That gives:
 *   - one [DROP] first line per reason, naming the draw's state;
 *   - one [DROP] line per report window, every non-zero reason with its
 *     batches per flip and share of the window's batches;
 *   - [DROP] WARNING when a DROPPED or SIMPLIFIED reason exceeds 1% of the
 *     window's batches, with up to four distinct states it hit.
 * diagnostics/jsrf_first_fault/drop_census.py reads them back as a table.
 *
 * The cost on the drawing path is one increment per batch (nv2a_drop_batch)
 * and one per flip; nothing else runs unless a draw is dropped. The executor
 * thread writes, the report thread reads: counts are monotonic, and the
 * report differences them against its last snapshot, so nothing is reset
 * under the writer. */
#include <stdint.h>

enum { NV2A_DROP_DROPPED = 0, NV2A_DROP_SIMPLIFIED = 1, NV2A_DROP_PARTIAL = 2 };

/* The draw's state as the first line and the WARNING print it: final
 * combiner CW0, the texture-shader modes (0x1E70), texture 0's format
 * (0x1B04), and (transform MODE | primitive << 8), plus the draw number. */
typedef struct { uint32_t cw0, texmodes, tex0_format, mode_prim, draw; } NV2ADropState;

/* Where the state comes from: the executor registers a filler, so callers
 * outside it (nv2a_texture_copy.c, nv2a_metal.m) need not know the method
 * array. NULL leaves the state zero. */
void nv2a_drop_set_state_source(void (*fill)(NV2ADropState *));

void nv2a_drop(int kind, const char *stage, const char *reason, uint32_t detail);
void nv2a_drop_batch(void);     /* every batch that reaches the rasteriser's entry */
void nv2a_drop_flip(void);
void nv2a_drop_report(const char *why);

/* For the tests: the total for a reason (0 if never seen). */
unsigned long long nv2a_drop_count(const char *reason);

#endif
