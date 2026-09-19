/* Finding a byte pattern in guest RAM, so "did the guest do it or did we?"
 * stops being an argument.
 *
 * This project asks that question constantly and has usually had to answer it
 * by reading generated C, which CLAUDE.md forbids for a reason. The concrete
 * case it was built for: a tutorial line renders as "llect 10 Spray Cans"
 * against a mission file that plainly contains "Collect 10 Spray Cans". Either
 * a guest routine built a short copy -- in which case the truncated bytes are
 * somewhere in RAM -- or the string is intact and the loss is in our layout or
 * iteration. One search decides it, and nothing in the tree could do it:
 * RECOMP_DUMP_VA prints dwords at addresses you already know.
 *
 * THE ANSWER IS ONLY WORTH ANYTHING WITH A POSITIVE CONTROL. A scan that finds
 * nothing because it is pointed at unmapped memory reads exactly like a scan
 * that finds nothing because the bytes are not there, and this tree has drawn
 * that false conclusion before. So the driver always scans for a pattern it
 * has just read out of guest memory itself, and says so on the same line.
 *
 * Pure on purpose, like wild_ptr.h beside it: these take the host base for
 * guest VA 0 and a range and read nothing else, which is what lets the test
 * hand them an ordinary malloc'd buffer instead of a running game.
 */

#ifndef JSRF_RAM_FIND_H
#define JSRF_RAM_FIND_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define JSRF_RF_MAX_PATTERN 64

/* One occurrence, as a guest VA. */
typedef struct {
    uint32_t va;
} JsrfRfHit;

/* Parse a human-written pattern into bytes.
 *
 * Plain characters stand for themselves, so a switch can carry readable text.
 * `\xNN` is a hex byte, which is what Shift-JIS and control codes need --
 * `$n\x81\x40` is a real thing to search for in this title. `\\` is a
 * backslash and `\;` a literal semicolon, since the driver splits on it.
 *
 * Returns the byte length written, or -1 if the spec is malformed or longer
 * than max. A zero-length pattern returns -1 rather than matching everywhere.
 */
int jsrf_rf_parse(const char *spec, unsigned char *out, int max);

/* Every occurrence of pat[0..len) in guest [lo,hi).
 *
 * `base` is the host address guest VA 0 maps to. Matches may start at any
 * byte -- no alignment is assumed, because the strings this looks for are not
 * aligned to anything. Returns the TOTAL number of occurrences, which may
 * exceed max_out; at most max_out hits are written to `out`, lowest address
 * first. Overlapping matches are counted separately.
 *
 * The caller passes a range that is mapped; this walks it unconditionally.
 */
uint32_t jsrf_rf_scan(const void *base, uint32_t lo, uint32_t hi,
                      const unsigned char *pat, uint32_t len,
                      JsrfRfHit *out, uint32_t max_out);

#ifdef __cplusplus
}
#endif

#endif /* JSRF_RAM_FIND_H */
