/* The title's own vertex programs, lifted out of default.xbe.
 *
 * WHY THIS EXISTS. Every test of the vertex path so far has run against one
 * captured program (vsh_capture.h, twelve slots) plus fixtures hand-built with
 * vsh_encode.h. Fixtures prove an opcode was reached; they prove nothing about
 * the shapes the title actually emits -- program length, how deep the register
 * pressure goes, how often a0-relative indexing is used, which opcodes pair in
 * one slot. A differential test against synthetic programs is the weakest
 * form of the test, and this is how to stop running it.
 *
 * WHAT WAS MEASURED, 16 Sep 2026, against the US dump:
 *
 *   - default.xbe contains NO vs.1.1 token streams. `0xFFFE0101` occurs zero
 *     times in 2,281,472 bytes. So this title does not hand D3D8 a token
 *     stream to translate at CreateVertexShader time; the NV2A microcode is
 *     in the image, already assembled.
 *   - It is stored as a table: each program is preceded by one word
 *     `(slots << 16) | 0x2078`, then `slots` 128-bit instruction slots. 127
 *     such headers exist; for 125 of them the declared length is exactly the
 *     length the FINAL bit terminates at, which is what makes them real rather
 *     than coincidence.
 *   - The scan below does NOT use those headers. It walks every 32-bit
 *     position, decodes with the production decoder (nv2a_vsh_parse) and keeps
 *     what parses. The headers are then used as a CHECK on the result -- see
 *     header_agreements in VshXbeScan. Two independent descriptions of the
 *     same boundary agreeing is the evidence; using the header to find the
 *     program and then reporting that the header was right would not be.
 *   - The positive control is vsh_capture.h. That program was captured from a
 *     running frame by RECOMP_VSH_TRACE, months before this scan existed, and
 *     it appears here byte for byte at file offset 0x18A888. A scan that
 *     cannot find a program known to be uploaded at runtime is a scan whose
 *     output means nothing, so a caller should assert on it.
 *
 * WHAT THIS IS NOT. It is the set of programs the image CONTAINS, not the set
 * a frame RUNS. A program in here may never be selected in the title; a
 * program the title builds at runtime -- if any -- is not in here at all. The
 * only claim is coverage of the encodings the title's own tools produced.
 *
 * The XBE cannot be vendored (see CLAUDE.md), so this reads it at run time and
 * a caller must degrade gracefully when the file is absent. Header-only, no
 * allocation, in the shape of vsh_encode.h.
 */
#ifndef JSRF_VSH_XBE_CORPUS_H
#define JSRF_VSH_XBE_CORPUS_H
#include "nv2a_vsh.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define VSH_XBE_MAX_PROGRAMS 512

typedef struct {
    uint32_t words[NV2A_VS_MAX_INSTRUCTIONS * 4];
    int      length;            /* slots */
    unsigned long offset;       /* byte offset in the file it came from */
    int      header_agreed;     /* a (slots<<16)|0x2078 word sits in front */
} VshXbeProgram;

typedef struct {
    int  count;                 /* distinct programs kept */
    int  runs;                  /* accepted runs before deduplication */
    int  header_agreements;     /* of `count`, how many carry a length header */
    int  headers_in_file;       /* 0x2078-shaped words found anywhere */
    long words_scanned;
    char path[1024];
} VshXbeScan;

/* Where the dump is. No default that points at a sibling checkout: CLAUDE.md
 * records that guessing a layout here wastes a session, so this names the
 * variables and returns NULL. */
static const char *vsh_xbe_default_path(char *buf, size_t size)
{
    const char *direct = getenv("JSRF_VSH_CORPUS_XBE");
    const char *dir = getenv("JSRF_GAME_DIR");
    if (direct && *direct) { snprintf(buf, size, "%s", direct); return buf; }
    if (dir && *dir) { snprintf(buf, size, "%s/default.xbe", dir); return buf; }
    return NULL;
}

/* Does this look like a vertex program rather than a run of data that happens
 * to decode? Three independent requirements, each of which arbitrary bytes
 * fail often: a terminating FINAL, at least one attribute read, and a write to
 * oPos -- by the output register or through its R12 alias. A vertex program
 * that writes no position is not one. */
static int vsh_xbe_plausible(const NV2AVshProgram *p)
{
    int pos = 0, i;
    if (!p->valid || !p->has_final || p->length < 4 || !p->inputs_read) return 0;
    /* A run must not START with a slot that does nothing.
     *
     * MEASURED, and it mattered: the word before the program at 0x18A718 is
     * 0xBF800000 sitting in what would be slot 0's third word, with both
     * opcode fields zero. The scan happily began a run there, produced a
     * thirteen-slot program with a dead slot in front, and swallowed the
     * twelve-slot program underneath -- which is the one RECOMP_VSH_TRACE
     * captured from a live frame. The corpus was then quietly not the title's
     * programs, and the only thing that noticed was the capture cross-check.
     * No assembler emits a leading double-NOP. */
    if (p->insns[0].mac_op == NV2A_VSH_MAC_NOP
        && p->insns[0].ilu_op == NV2A_VSH_ILU_NOP) return 0;
    for (i = 0; i < p->length; ++i) {
        const NV2AVshInstruction *s = &p->insns[i];
        if (s->mac_dst.output_reg == NV2A_VSH_OUT_POS && s->mac_dst.output_mask) pos = 1;
        if (s->ilu_dst.output_reg == NV2A_VSH_OUT_POS && s->ilu_dst.output_mask) pos = 1;
        if (s->mac_dst.temp_reg == 12 && s->mac_dst.write_mask) pos = 1;
        if (s->ilu_dst.temp_reg == 12 && s->ilu_dst.write_mask) pos = 1;
    }
    return pos;
}

/* Returns the number of distinct programs written to `out`, or -1 if the file
 * could not be read. Scans every 32-bit position -- NOT every 128-bit slot.
 * The first version stepped by four words after a failed run and silently lost
 * the first instruction of every program in the table, because the table's
 * one-word length header puts the programs on the other phase. That cost two
 * programs their MOV into R0 and would have made this a corpus of subtly
 * different programs than the title's. */
static int vsh_xbe_scan(const char *path, VshXbeProgram *out, int max,
                        VshXbeScan *info)
{
    FILE *f;
    long bytes, n, i;
    uint32_t *w;
    int count = 0, runs = 0;

    memset(info, 0, sizeof(*info));
    snprintf(info->path, sizeof(info->path), "%s", path ? path : "(none)");
    if (!path) return -1;
    f = fopen(path, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END); bytes = ftell(f); fseek(f, 0, SEEK_SET);
    if (bytes <= 0) { fclose(f); return -1; }
    w = (uint32_t *)malloc((size_t)bytes);
    if (!w) { fclose(f); return -1; }
    if (fread(w, 1, (size_t)bytes, f) != (size_t)bytes) { free(w); fclose(f); return -1; }
    fclose(f);
    n = bytes / 4;
    info->words_scanned = n;

    for (i = 0; i + 1 < n; ++i)
        if ((w[i] & 0xFFFFu) == 0x2078u && (w[i] >> 16) >= 1
            && (w[i] >> 16) <= NV2A_VS_MAX_INSTRUCTIONS)
            ++info->headers_in_file;

    for (i = 0; i + 4 <= n; ) {
        long j = i;
        int L = 0, fin = 0, b, dup = 0;
        NV2AVshProgram p;
        /* The unused first word of each slot is the cheap gate: it is zero in
         * every program captured from a real upload and in every program in
         * the table. A run of slots whose first word is nonzero is not a
         * program in this encoding. */
        if (w[i] != 0 || (w[i+1] | w[i+2] | w[i+3]) == 0) { ++i; continue; }
        while (j + 4 <= n && w[j] == 0 && (w[j+1] | w[j+2] | w[j+3]) != 0
               && L < NV2A_VS_MAX_INSTRUCTIONS) {
            ++L;
            if (w[j+3] & 1u) { fin = 1; break; }
            j += 4;
        }
        if (!fin || !nv2a_vsh_parse(w + i, L, &p) || !vsh_xbe_plausible(&p)) { ++i; continue; }
        ++runs;
        for (b = 0; b < count; ++b)
            if (out[b].length == L && !memcmp(out[b].words, w + i, (size_t)L * 16)) { dup = 1; break; }
        if (!dup && count < max) {
            memcpy(out[count].words, w + i, (size_t)L * 16);
            out[count].length = L;
            out[count].offset = (unsigned long)i * 4;
            out[count].header_agreed =
                i > 0 && w[i-1] == (((uint32_t)L << 16) | 0x2078u);
            if (out[count].header_agreed) ++info->header_agreements;
            ++count;
        }
        i = j + 4;
    }
    free(w);
    info->count = count;
    info->runs = runs;
    return count;
}

#endif
