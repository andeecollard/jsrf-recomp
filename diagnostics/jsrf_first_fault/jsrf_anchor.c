/* The JSRF state anchor's field layout, and how to read a disagreement.
 *
 * Split out of main.c so jsrf_anchor_test links THIS function rather than a
 * transcription of it. A describer that drifts from the packing it describes
 * would mislabel the field that moved, which is worse than the bare hex it
 * replaced.
 */
#include "jsrf_anchor.h"
#include <stdio.h>

/* NAME THE FIELD THAT MOVED, because "was 1E000003, now 1E000004" does not.
 *
 * The anchor packs four pieces of guest state into one word so it fits the
 * recording format. That is fine for COMPARING and useless for READING: a
 * misalignment report that hands back two hex words leaves whoever is already
 * debugging something else to decode a bitfield by hand. The input layer
 * cannot help -- it is title-agnostic on purpose -- so JSRF registers this.
 *
 * PROVENANCE, PER FIELD. The distinction that matters is not "where is it"
 * but "how do we know", because a value borrowed from elsewhere and then
 * verified against its own source proves nothing:
 *
 *   seq       CActSequence::m_dwNextMethod, via jsrf_seq_object's validated
 *             derefs. INDEPENDENT -- the sequence table was read out of guest
 *             RAM at 0x0020D2B8 and re-validated every run, and it agrees with
 *             the decompilation's eSEQUENCE ordering rather than deriving
 *             from it.
 *   chapter   CSaveData +0x04. INDEPENDENT -- GetReturnChapterNo reads
 *             [ecx+4] in the generated code.
 *   mission   CSaveData +0x08. INDEPENDENT -- GetReturnMissionNo reads
 *             [ecx+8] in the generated code.
 *   minutes   CSaveData +0x10, seconds/60. UNKNOWN -- the offset is believed
 *             from the same struct walk that gave the two above, but no
 *             instruction reading it has been quoted in this corpus. Named as
 *             unknown-provenance rather than asserted; it is the slow counter
 *             and is compared with slack, so it is also the field least able
 *             to mislead.
 *
 * Only differing fields are printed. The three identity fields are exact; the
 * minute counter is the one the input layer compares with tolerance, so it is
 * labelled as slow to stop a one-minute gap reading as a real divergence. */
void jsrf_pad_anchor_describe(unsigned long rec, unsigned long live,
                                     char *out, unsigned long n)
{
    struct { const char *name; unsigned shift, mask; int slow; } f[] = {
        { "sequence", 24, 0xFFu,   0 },
        { "chapter",  20, 0x0Fu,   0 },
        { "mission",  16, 0x0Fu,   0 },
        { "minutes",   0, 0xFFFFu, 1 },
    };
    unsigned long used = 0;
    unsigned i;

    if (!n) return;
    out[0] = 0;
    for (i = 0; i < sizeof f / sizeof f[0]; ++i) {
        unsigned long a = (rec >> f[i].shift) & f[i].mask;
        unsigned long b = (live >> f[i].shift) & f[i].mask;
        int k;
        if (a == b) continue;
        k = snprintf(out + used, (size_t)(n - used), "%s%s %lu->%lu%s",
                     used ? ", " : "", f[i].name, a, b,
                     f[i].slow ? " (slow counter)" : "");
        if (k < 0 || (unsigned long)k >= n - used) { out[n - 1] = 0; return; }
        used += (unsigned long)k;
    }
    /* Anchors can differ in a bit no field claims -- a layout change, or a
     * recording from a build that packed them differently. Saying so is more
     * useful than an empty phrase that reads like "nothing differs". */
    if (!used && rec != live)
        snprintf(out, (size_t)n, "no named field (layout mismatch?)");
}
