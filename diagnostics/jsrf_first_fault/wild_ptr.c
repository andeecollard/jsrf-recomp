/* See wild_ptr.h for why this exists. Nothing here runs unless the title has
 * already faulted, or the operator asked for the self-test. */

#include "wild_ptr.h"

#include <stddef.h>

const char *const jsrf_wp_reg_names[JSRF_WP_NREG] = {
    "EAX", "ECX", "EDX", "EBX", "ESI", "EDI", "EBP", "ESP"
};

int jsrf_wp_attribute(uint32_t fault_va, const uint32_t regs[JSRF_WP_NREG],
                      uint32_t max_disp, int *reg_out, uint32_t *disp_out)
{
    int best = -1;
    uint32_t best_disp = 0;
    int i;

    if (!regs) return 0;

    for (i = 0; i < JSRF_WP_NREG; ++i) {
        uint32_t disp;
        /* Wrapping subtraction on purpose. The guest is 32-bit and a base
         * register close to 0xFFFFFFFF addresses low memory by wrapping, so
         * computing the displacement in 64 bits would miss exactly the case
         * where the base is most obviously wrong. */
        disp = fault_va - regs[i];
        if (disp > max_disp) continue;
        if (best < 0 || disp < best_disp) {
            best = i;
            best_disp = disp;
        }
    }
    if (best < 0) return 0;
    if (reg_out) *reg_out = best;
    if (disp_out) *disp_out = best_disp;
    return 1;
}

uint32_t jsrf_wp_scan(const void *base, uint32_t lo, uint32_t hi,
                      uint32_t value, JsrfWpHit *out, uint32_t max_out,
                      uint32_t *runs_out, uint32_t *longest_run_out)
{
    const unsigned char *bytes = (const unsigned char *)base;
    uint32_t total = 0, runs = 0, longest = 0, written = 0;
    uint32_t va;

    if (runs_out) *runs_out = 0;
    if (longest_run_out) *longest_run_out = 0;
    if (!bytes || hi <= lo) return 0;

    lo = (lo + 3u) & ~3u;
    if (hi < 4u || lo > hi - 4u) return 0;

    for (va = lo; va <= hi - 4u; ) {
        uint32_t word;
        /* Byte-wise so the reader does not depend on the host tolerating an
         * unaligned 32-bit load, and so the unit test can hand in a plain
         * buffer at any address. The range is 4-aligned either way. */
        word = (uint32_t)bytes[va]
             | ((uint32_t)bytes[va + 1] << 8)
             | ((uint32_t)bytes[va + 2] << 16)
             | ((uint32_t)bytes[va + 3] << 24);
        if (word != value) { va += 4u; continue; }

        {
            uint32_t start = va, run = 0;
            while (va <= hi - 4u) {
                word = (uint32_t)bytes[va]
                     | ((uint32_t)bytes[va + 1] << 8)
                     | ((uint32_t)bytes[va + 2] << 16)
                     | ((uint32_t)bytes[va + 3] << 24);
                if (word != value) break;
                ++run;
                va += 4u;
            }
            total += run;
            ++runs;
            if (run > longest) longest = run;
            if (out && written < max_out) {
                out[written].va = start;
                out[written].run = run;
                ++written;
            }
        }
    }

    if (runs_out) *runs_out = runs;
    if (longest_run_out) *longest_run_out = longest;
    return total;
}

uint32_t jsrf_wp_parents(const void *base, uint32_t esp, uint32_t top,
                         uint32_t ram_lo, uint32_t ram_hi, uint32_t value,
                         uint32_t max_field, JsrfWpParent *out,
                         uint32_t max_out)
{
    const unsigned char *bytes = (const unsigned char *)base;
    uint32_t found = 0, written = 0;
    uint32_t slot;

    if (!bytes || top <= esp || top < 4u) return 0;
    esp = (esp + 3u) & ~3u;
    if (esp > top - 4u) return 0;

    for (slot = esp; slot <= top - 4u; slot += 4u) {
        uint32_t node = (uint32_t)bytes[slot]
                      | ((uint32_t)bytes[slot + 1] << 8)
                      | ((uint32_t)bytes[slot + 2] << 16)
                      | ((uint32_t)bytes[slot + 3] << 24);
        uint32_t k;

        /* A stack slot is only a candidate object if it could be one: inside
         * the arena and 4-aligned. Without the alignment test every saved
         * float and every small integer becomes an "object" and the report
         * fills with noise that hides the one real answer. */
        if (node < ram_lo || node >= ram_hi) continue;
        if (node & 3u) continue;
        if (ram_hi - node < max_field) continue;

        for (k = 0; k + 4u <= max_field; k += 4u) {
            uint32_t field = (uint32_t)bytes[node + k]
                           | ((uint32_t)bytes[node + k + 1] << 8)
                           | ((uint32_t)bytes[node + k + 2] << 16)
                           | ((uint32_t)bytes[node + k + 3] << 24);
            if (field != value) continue;
            ++found;
            if (out && written < max_out) {
                out[written].node = node;
                out[written].field = k;
                out[written].via = slot;
                ++written;
            }
            break;   /* one report per object: the first field that matches */
        }
    }
    return found;
}
