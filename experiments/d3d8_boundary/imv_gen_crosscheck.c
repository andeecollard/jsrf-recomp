/* G42b cross-check: the RECOMPILED guest's own 0x190750 (WORLD*VIEW),
 * 0x190A30 (the inverse) and 0x190930 (rsqrt), extracted verbatim from a gen
 * tree, run side by side with the transcription in
 * src/nv2a/d3d8_ff_vertex_state.c on random matrices, word for word. Not
 * built by CMake: the gen is not in git. Rerun it after a regeneration or a
 * translator change to the x87/SSE lowering. Build in a scratch directory:
 *
 *   G=~/jsrf-build/jsrf-first-fault/gen; R=<repo>
 *   f=$(grep -l 'void sub_00190A30(void)$' $G/recomp_*.c)
 *   for a in 00190A30:g_inv 00190930:g_rsq 00190750:g_mul; do
 *     awk "/^void sub_${a%%:*}\\(void\\)/,/^}/" $f > ${a##*:}.c; done
 *   cc -O2 -std=gnu11 -w -I. -I$R/src/nv2a $R/experiments/d3d8_boundary/imv_gen_crosscheck.c \
 *      $R/src/nv2a/d3d8_ff_vertex_state.c -o xc -lm && ./xc 3000000
 *
 * The macros below stand in for recomp_types.h's with the same semantics
 * (x87 stack in double, SSE lanes in float, FCMP/fnstsw/parity as there).
 * 24 Sep 2026, gen at ~/jsrf-build/jsrf-first-fault/gen: 3,000,000 cases,
 * 0 inverse and 0 product mismatches (512,362 singular, both sides -1);
 * control -- the transcription with NORMALIZENORMALS' sense flipped --
 * differs from the guest in 2,487,408 of the 2,487,638 regular cases. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "d3d8_ff_vertex_state.h"
static uint8_t g_mem[0x400000];
#define XBOX_PTR(a) ((uintptr_t)(g_mem + ((uint32_t)(a) & 0x3FFFFFu)))
#define MEM32(addr)  (*(volatile uint32_t *)XBOX_PTR(addr))
#define MEMF(addr)   (*(volatile float    *)XBOX_PTR(addr))
#define RECOMP_MEM_WRITE32(pc, fn, addr, v) (MEM32(addr) = (uint32_t)(v))
#define RECOMP_MEM_WRITEF(pc, fn, addr, v) (MEMF(addr) = (float)(v))
#define PUSH32(sp, val) do { uint32_t _pv = (uint32_t)(val); (sp) -= 4; MEM32(sp) = _pv; } while (0)
#define POP32(sp, dst) do { (dst) = MEM32(sp); (sp) += 4; } while (0)
#define RECOMP_ABI_CALL(va, fn) (fn)()
#define RECOMP_FCMP(a, b)     (((a) != (a) || (b) != (b)) ? 2 : (a) < (b) ? -1 : (a) > (b) ? 1 : 0)
#define RECOMP_FCMP_CC(c) ((uint16_t)((c)==2 ? 0x4500u : (c)<0 ? 0x0100u : (c)>0 ? 0u : 0x4000u))
static inline int recomp_parity8(uint32_t x) { x &= 0xFFu; x ^= x >> 4; x ^= x >> 2; x ^= x >> 1; return (int)(~x & 1u); }
#define RECOMP_PARITY8(x) recomp_parity8((uint32_t)(x))
#define HI8(r)  ((uint8_t)(((r) >> 8) & 0xFF))
#define TEST_Z(a, b)  (((uint32_t)(a) & (uint32_t)(b)) == 0)
typedef union { float f[4]; uint32_t u[4]; } RecompXmm;
static RecompXmm xmm0, xmm1, xmm2, xmm3, xmm4, xmm5;
static inline RecompXmm XMM_MEM(uint32_t a) { RecompXmm r; for (int i = 0; i < 4; ++i) r.u[i] = MEM32(a + 4u * i); return r; }
static inline RecompXmm XMM_ADD(RecompXmm a, RecompXmm b) { RecompXmm r; for (int i = 0; i < 4; ++i) r.f[i] = a.f[i] + b.f[i]; return r; }
static inline RecompXmm XMM_MUL(RecompXmm a, RecompXmm b) { RecompXmm r; for (int i = 0; i < 4; ++i) r.f[i] = a.f[i] * b.f[i]; return r; }
static inline RecompXmm XMM_SHUFFLE(RecompXmm a, RecompXmm b, uint32_t imm) { RecompXmm r;
    r.u[0] = a.u[imm & 3u]; r.u[1] = a.u[(imm >> 2) & 3u]; r.u[2] = b.u[(imm >> 4) & 3u]; r.u[3] = b.u[(imm >> 6) & 3u]; return r; }
#define XMM_STORE(a, v) do { RecompXmm _v = (v); for (int _i = 0; _i < 4; ++_i) MEM32((a) + 4u * _i) = _v.u[_i]; } while (0)
static uint32_t eax, ecx, edx, esp;
static double g_fp_stack[8]; static unsigned g_fp_top; static int g_fp_cmp; static uint16_t g_fp_cc;
void sub_00190930(void);
#include "g_rsq.c"
#include "g_inv.c"
#include "g_mul.c"

static uint64_t rng = 0x9E3779B97F4A7C15ull;
static uint32_t rnd(void) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return (uint32_t)rng; }
static float rf(int kind)
{
    uint32_t r = rnd();
    switch (kind) {
    case 0: return ((float)(r % 2000001u) - 1000000.0f) / 250000.0f;        /* -4..4 */
    case 1: return (r & 1) ? 0.0f : ((float)(r % 2001u) - 1000.0f) / 7.0f;   /* sparse */
    default: { uint32_t w = (r & 0x807FFFFFu) | ((uint32_t)(0x60u + (rnd() % 0x40u)) << 23); float f; memcpy(&f, &w, 4); return f; }
    }
}
int main(int argc, char **argv)
{
    unsigned n = argc > 1 ? (unsigned)atoi(argv[1]) : 1000000u, bad = 0, sing = 0, badmul = 0, ctl_n = 0, ctl_diff = 0;
    uint32_t k1 = 0x3EF0A3D7u, k2 = 0x3FBC28F6u, k3 = 0x40400000u, k4 = 0x3F000000u, k0 = 0;
    memcpy(&g_mem[0x22E574], &k1, 4); memcpy(&g_mem[0x22E578], &k2, 4);
    memcpy(&g_mem[0x1CC624], &k3, 4); memcpy(&g_mem[0x1C4550], &k4, 4); memcpy(&g_mem[0x1C43D0], &k0, 4);
    for (unsigned it = 0; it < n; ++it) {
        uint32_t m[16], a[16], b[16], want[16], got[16], mul[16];
        int kind = it % 3, scale = (it >> 2) & 1, rg, rt;
        for (int i = 0; i < 16; ++i) { float f = rf(kind); memcpy(&a[i], &f, 4); f = rf(kind); memcpy(&b[i], &f, 4); }
        if (it % 7 == 0) { a[3] = a[7] = a[11] = 0; a[15] = 0x3F800000u; b[3] = b[7] = b[11] = 0; b[15] = 0x3F800000u; }
        if (it % 101 == 0) for (int i = 0; i < 4; ++i) a[4 + i] = a[i];     /* singular */
        /* the guest's product, 0x190750(out=0x3000, a=0x3100, b=0x3200) */
        for (int i = 0; i < 16; ++i) { MEM32(0x3100 + 4 * i) = a[i]; MEM32(0x3200 + 4 * i) = b[i]; }
        esp = 0x8000; PUSH32(esp, 0x3200); PUSH32(esp, 0x3100); PUSH32(esp, 0x3000); PUSH32(esp, 0xDEAD);
        sub_00190750(); if (esp != 0x8000) { printf("mul esp %x\n", esp); return 1; }
        for (int i = 0; i < 16; ++i) m[i] = MEM32(0x3000 + 4 * i);
        d3d8_ff_matmul(mul, a, b);
        if (memcmp(mul, m, sizeof m)) badmul++;
        /* the guest's inverse, 0x190A30(out=0x4000, in=0x3000, scale) */
        for (int i = 0; i < 16; ++i) MEM32(0x4000 + 4 * i) = 0xA5A5A5A5u;
        esp = 0x8000; PUSH32(esp, scale); PUSH32(esp, 0x3000); PUSH32(esp, 0x4000); PUSH32(esp, 0xBEEF);
        g_fp_top = 0; sub_00190A30(); rg = (int)eax;
        if (esp != 0x8000) { printf("esp %x\n", esp); return 1; }
        for (int i = 0; i < 16; ++i) want[i] = MEM32(0x4000 + 4 * i);
        for (int i = 0; i < 16; ++i) got[i] = 0xA5A5A5A5u;
        rt = d3d8_ff_inverse(got, m, scale);
        if (rt == -1) sing++;
        if (rt != (rg == -1 ? -1 : 0) || memcmp(want, got, sizeof want)) {
            if (bad++ < 5) { printf("MISMATCH it %u scale %d rg %d rt %d\n", it, scale, rg, rt);
                for (int i = 0; i < 16; ++i) printf("  %2d %08X %08X\n", i, want[i], got[i]); }
        }
        /* positive control: the other NORMALIZENORMALS sense must differ */
        if (rt == 0) { ctl_n++; d3d8_ff_inverse(got, m, !scale); if (memcmp(want, got, sizeof want)) ctl_diff++; }
    }
    printf("%u cases: inverse mismatches %u, product mismatches %u, singular %u;"
           " control (scale flipped) differs in %u of %u\n", n, bad, badmul, sing, ctl_diff, ctl_n);
    return bad || badmul || ctl_diff * 10u < ctl_n * 9u;
}
