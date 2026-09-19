/* Naming the object that held a wild pointer, after the fault.
 *
 * The crash reporter can already say where the host died, what the guest
 * registers were, and which code pointers were on the guest stack. On a
 * scene-graph walk that is not enough: the walker dies dereferencing a child
 * pointer it was handed, so the register file describes the VICTIM and says
 * nothing about the node whose field was wrong. Every such report so far has
 * ended at "EDI was garbage", which is the question, not the answer.
 *
 * What is missing is one step backwards -- the object that held the bad value,
 * the field offset it held it at, and whether that value is a one-off or
 * belongs to a large run of identical words somewhere it was never a pointer
 * at all. All three are recoverable AFTER the fault from guest memory that is
 * still intact, so none of this costs anything while the title is running.
 *
 * The routines below are deliberately pure: they take the host base for guest
 * VA 0 and a range, and they read nothing else. That is what lets the test
 * hand them an ordinary malloc'd buffer and check the answers against a graph
 * it built itself, rather than against a crash nobody can reproduce on demand.
 */

#ifndef JSRF_WILD_PTR_H
#define JSRF_WILD_PTR_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The guest register file, in the order the crash report already prints it.
 * Keeping the order identical means the index this header hands back can be
 * read straight off the GUEST REGISTERS line above it. */
enum {
    JSRF_WP_EAX = 0, JSRF_WP_ECX, JSRF_WP_EDX, JSRF_WP_EBX,
    JSRF_WP_ESI, JSRF_WP_EDI, JSRF_WP_EBP, JSRF_WP_ESP,
    JSRF_WP_NREG
};
extern const char *const jsrf_wp_reg_names[JSRF_WP_NREG];

/* One place in guest memory holding the value, and how many consecutive
 * words from there hold it. The run length is the whole point: a corrupted
 * pointer field is a single isolated word, while a decoded colour or material
 * buffer is thousands in a row. One number separates the two hypotheses. */
typedef struct {
    uint32_t va;
    uint32_t run;
} JsrfWpHit;

/* An object that holds the value, and where. `via` records the guest address
 * the candidate object pointer was itself read from, so a reader can tell a
 * parent recovered from the stack from one recovered any other way. */
typedef struct {
    uint32_t node;
    uint32_t field;
    uint32_t via;
} JsrfWpParent;

/* Which register could have carried `fault_va` as base + displacement.
 *
 * A translated read is MEM32(reg + k) with a small constant k, so the register
 * that carried the fault is the one whose value sits just below the faulting
 * address. Scanning for the SMALLEST such displacement matters: with a wild
 * value like 0xFF555555 several registers often hold the same garbage, and the
 * one with displacement 4 is the base while one with displacement 0x3F0 is a
 * coincidence. Ties go to the lowest displacement, then to register order.
 *
 * Returns 1 and fills *reg_out / *disp_out when a register matches within
 * max_disp bytes, 0 when none does. Either out pointer may be NULL.
 */
int jsrf_wp_attribute(uint32_t fault_va, const uint32_t regs[JSRF_WP_NREG],
                      uint32_t max_disp, int *reg_out, uint32_t *disp_out);

/* Every 4-aligned word in [lo,hi) equal to `value`.
 *
 * `base` is the host address that guest VA 0 maps to. Returns the total number
 * of matching WORDS, which is the figure that decides whether the value is
 * data or a corrupted pointer. At most max_out runs are written to `out`,
 * oldest first; *runs_out receives the total number of distinct runs and
 * *longest_run_out the longest, both optional. The caller is responsible for
 * passing a range that is mapped -- this walks it unconditionally.
 */
uint32_t jsrf_wp_scan(const void *base, uint32_t lo, uint32_t hi,
                      uint32_t value, JsrfWpHit *out, uint32_t max_out,
                      uint32_t *runs_out, uint32_t *longest_run_out);

/* Candidate holders reachable from a guest stack.
 *
 * Walks [esp,top) as 32-bit words. Any word that is itself a plausible guest
 * object pointer (inside [ram_lo,ram_hi), 4-aligned) is treated as an object
 * and its first max_field bytes are searched for `value`. A hit means "this
 * object, which the faulting thread still had a reference to, holds the wild
 * value at this offset" -- which for a recursive walker is the parent node,
 * because the walker saved its own `this` on the stack before recursing.
 *
 * Each distinct object is reported once, at the first offset that matches.
 * Returns the number of candidates found, which may exceed max_out.
 */
uint32_t jsrf_wp_parents(const void *base, uint32_t esp, uint32_t top,
                         uint32_t ram_lo, uint32_t ram_hi, uint32_t value,
                         uint32_t max_field, JsrfWpParent *out,
                         uint32_t max_out);

#ifdef __cplusplus
}
#endif

#endif /* JSRF_WILD_PTR_H */
