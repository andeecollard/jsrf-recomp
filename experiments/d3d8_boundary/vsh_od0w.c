/* G38b: in each of the title's vertex programs, where does oD0.w come from?
 *
 * Walks the program, finds the LAST instruction slot that writes oD0 with the
 * w bit, and traces the w component back: through MOV/MUL/MAD/ADD/MIN/MAX on
 * the w lane, through temporaries, to what it bottoms out in -- constant
 * registers, input registers, or something computed (dot products, ILU ops,
 * relative addressing). Prints a class per program and a histogram.
 *
 *   cc -O1 -Isrc/nv2a -Idiagnostics/jsrf_first_fault experiments/d3d8_boundary/vsh_od0w.c \
 *      src/nv2a/nv2a_vsh.c -lm -o /tmp/vsh_od0w
 *   JSRF_GAME_DIR=<dump> /tmp/vsh_od0w
 *
 * Classes: CONST (only constant registers), INPUT (only input registers,
 * possibly times constants), NONE (oD0.w never written: the hardware default),
 * COMPUTED (anything else: a dot product, an ILU op, a0-relative constants).
 */
#include "vsh_xbe_corpus.h"
#include <stdint.h>

static VshXbeProgram progs[VSH_XBE_MAX_PROGRAMS];

enum { K_CONST = 1, K_INPUT = 2, K_COMPUTED = 4 };

static uint32_t fnv(const uint32_t *w, int n)
{ uint32_t h = 2166136261u; for (int i = 0; i < n; ++i) { h ^= w[i]; h *= 16777619u; } return h; }

static int comp_of(const NV2AVshSwizzle *s, int lane)
{ return lane == 0 ? s->x : lane == 1 ? s->y : lane == 2 ? s->z : s->w; }

/* Trace component `lane` of temp `reg` as it stood just before slot `before`. */
static int trace_temp(const NV2AVshProgram *p, int before, int reg, int lane, int depth);

static int trace_src(const NV2AVshProgram *p, int slot, const NV2AVshSrcOperand *s, int lane, int depth)
{
    int c = comp_of(&s->swizzle, lane);
    if (s->reg_type == NV2A_VSH_REG_CONST) return s->rel_addr ? K_COMPUTED | K_CONST : K_CONST;
    if (s->reg_type == NV2A_VSH_REG_INPUT) return K_INPUT;
    return trace_temp(p, slot, s->reg_index, c, depth + 1);
}

static int trace_mac(const NV2AVshProgram *p, int slot, int lane, int depth)
{
    const NV2AVshInstruction *in = &p->insns[slot];
    switch (in->mac_op) {
    case NV2A_VSH_MAC_MOV: return trace_src(p, slot, &in->mac_src[0], lane, depth);
    case NV2A_VSH_MAC_MUL: case NV2A_VSH_MAC_MIN: case NV2A_VSH_MAC_MAX:
        return trace_src(p, slot, &in->mac_src[0], lane, depth) | trace_src(p, slot, &in->mac_src[1], lane, depth);
    case NV2A_VSH_MAC_ADD:
        return trace_src(p, slot, &in->mac_src[0], lane, depth) | trace_src(p, slot, &in->mac_src[2], lane, depth);
    case NV2A_VSH_MAC_MAD:
        return trace_src(p, slot, &in->mac_src[0], lane, depth) | trace_src(p, slot, &in->mac_src[1], lane, depth)
             | trace_src(p, slot, &in->mac_src[2], lane, depth);
    default: return K_COMPUTED;
    }
}

static int trace_temp(const NV2AVshProgram *p, int before, int reg, int lane, int depth)
{
    if (depth > 64) return K_COMPUTED;
    for (int i = before - 1; i >= 0; --i) {
        const NV2AVshInstruction *in = &p->insns[i];
        uint8_t bit = (uint8_t)(1u << (3 - lane));
        if (in->mac_op != NV2A_VSH_MAC_NOP && in->mac_dst.temp_reg == reg && (in->mac_dst.write_mask & bit))
            return trace_mac(p, i, lane, depth);
        if (in->ilu_op != NV2A_VSH_ILU_NOP && in->ilu_dst.temp_reg == reg && (in->ilu_dst.write_mask & bit))
            return in->ilu_op == NV2A_VSH_ILU_MOV ? trace_src(p, i, &in->ilu_src, lane, depth) : K_COMPUTED;
    }
    return K_COMPUTED;   /* read before written: undefined, treat as unknown */
}

int main(void)
{
    char path[1024]; VshXbeScan info;
    int n = vsh_xbe_scan(vsh_xbe_default_path(path, sizeof path), progs, VSH_XBE_MAX_PROGRAMS, &info);
    int hist[8] = {0}, none = 0;
    if (n <= 0) { fprintf(stderr, "no corpus (set JSRF_GAME_DIR)\n"); return 1; }
    for (int k = 0; k < n; ++k) {
        NV2AVshProgram p;
        if (nv2a_vsh_parse(progs[k].words, progs[k].length * 4, &p) < 0) continue;
        int last = -1, from_mac = 0;
        for (int i = 0; i < p.length; ++i) {
            if (p.insns[i].mac_op != NV2A_VSH_MAC_NOP && p.insns[i].mac_dst.output_reg == NV2A_VSH_OUT_D0 && (p.insns[i].mac_dst.output_mask & 1)) { last = i; from_mac = 1; }
            if (p.insns[i].ilu_op != NV2A_VSH_ILU_NOP && p.insns[i].ilu_dst.output_reg == NV2A_VSH_OUT_D0 && (p.insns[i].ilu_dst.output_mask & 1)) { last = i; from_mac = 0; }
        }
        int cls;
        if (last < 0) { ++none; cls = 0; }
        else if (from_mac) cls = trace_mac(&p, last, 3, 0);
        else cls = p.insns[last].ilu_op == NV2A_VSH_ILU_MOV ? trace_src(&p, last, &p.insns[last].ilu_src, 3, 0) : K_COMPUTED;
        if (last >= 0) ++hist[cls & 7];
        printf("prog %3d fnv=%08X slots=%2d oD0.w: %s%s%s%s\n", k, fnv(progs[k].words, progs[k].length * 4), p.length,
               last < 0 ? "NONE" : "", (cls & K_COMPUTED) ? "COMPUTED " : "", (cls & K_INPUT) ? "INPUT " : "", (cls & K_CONST) ? "CONST" : "");
    }
    printf("programs=%d none=%d const-only=%d input(+const)=%d computed=%d\n", n, none,
           hist[K_CONST], hist[K_INPUT] + hist[K_INPUT | K_CONST],
           hist[4] + hist[5] + hist[6] + hist[7]);
    return 0;
}
