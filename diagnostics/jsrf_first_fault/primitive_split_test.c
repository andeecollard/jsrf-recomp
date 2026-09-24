/* G54: IS A PRIMITIVE SPLIT ACROSS METHOD BURSTS ONE PRIMITIVE?
 *
 * The police scenes' "unaccounted" draws (up to 10% of a window) raised the
 * possibility that a TRIANGLE_STRIP delivered in several ARRAY_ELEMENT /
 * DRAW_ARRAYS bursts inside one Begin/End was assembled per burst -- losing
 * the triangles at the seams and leaving a 1-2 index remnant that the
 * rasteriser drops. xemu accumulates every element until END. So must this:
 *
 *   - a strip of 11 indices as ARRAY_ELEMENT16 x3 (6), ARRAY_ELEMENT32 (1,
 *     D3D's odd tail) and ARRAY_ELEMENT16 x2 (4), each a separate method
 *     call, is ONE batch of 11 indices -> 9 triangles by the strip rule;
 *   - a triangle list as two DRAW_ARRAYS runs is one batch of 6;
 *   - neither produces a "fewer than three indices" drop.
 * Controls: a genuine 2-index line batch IS counted as that drop; a Begin
 * whose only vertices are immediate-mode position writes is counted as the
 * immediate-mode drop; a Begin over an open batch is counted too. */
#define _POSIX_C_SOURCE 200809L
#include "nv2a_regs.h"
#include "nv2a_drop.h"
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t ram[32768];
static uint8_t gpu_regs[0x800000];
ptrdiff_t xbox_GetMemoryOffset(void) { return (ptrdiff_t)ram; }
void *xbox_GpuMemoryRange(uint32_t address, size_t bytes) {
    return (uint64_t)address + bytes <= sizeof(ram) ? (uint8_t *)ram + address : NULL;
}
const uint8_t *xbox_Nv2aRegisterMemory(void) { return gpu_regs; }
double xbox_TraceSeconds(void) { return 0.0; }
int xbox_HeapDescribe(uint32_t xbox_va, char *buf, size_t size)
{ (void)xbox_va; if (buf && size) snprintf(buf, size, "no heap in this harness"); return 0; }
void xbox_FramebufferWindowSet(uint32_t a, uint32_t p) { (void)a; (void)p; }
void xbox_FramebufferWindowStart(void) {}

void nv2a_pb_exec_method(uint32_t subch, uint32_t method, uint32_t param);
uint32_t nv2a_pb_exec_last_batch(uint32_t *prim);

static int fails;
#define CHECK(c, ...) do { if (c) { printf("ok: "); printf(__VA_ARGS__); printf("\n"); } \
                           else { ++fails; printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); } } while (0)
static void put(uint32_t m, uint32_t p) { nv2a_pb_exec_method(0, m, p); }
static unsigned strip_triangles(unsigned n) { return n >= 3 ? n - 2 : 0; }

#define SHORT "fewer than three indices (a point or line batch)"
#define IMM "immediate-mode vertices (SET_VERTEX_DATA inside Begin/End) are not emitted"
#define OPEN "Begin while a batch was still open (the open batch is lost)"

int main(void)
{
    uint32_t prim = 0, n;
    unsigned long long short0;
    put(NV097_SET_SURFACE_CLIP_HORIZONTAL, 64u << 16);
    put(NV097_SET_SURFACE_CLIP_VERTICAL, 48u << 16);
    put(NV097_SET_SURFACE_PITCH, 256);

    /* A strip of 11 in five method calls. */
    put(0x17FC, 6);                                          /* BEGIN TRIANGLE_STRIP */
    put(0x1800, 0x00010000u); put(0x1800, 0x00030002u); put(0x1800, 0x00050004u);
    put(0x1808, 6);                                          /* the odd tail, 32-bit */
    put(0x1800, 0x00080007u); put(0x1800, 0x000A0009u);
    put(0x17FC, 0);                                          /* END */
    n = nv2a_pb_exec_last_batch(&prim);
    CHECK(n == 11 && prim == 6, "a strip split over 5 method calls is ONE batch of 11 indices (%u, prim %u)", n, prim);
    CHECK(strip_triangles(n) == 9, "which the strip rule assembles into N-2 = 9 triangles");
    CHECK(nv2a_drop_count(SHORT) == 0, "and leaves no short remnant");

    /* A triangle list as two DRAW_ARRAYS runs. */
    put(0x17FC, 5);
    put(0x1810, 0x03000000u);                                /* start 0, count 4 */
    put(0x1810, 0x01000004u);                                /* start 4, count 2 */
    put(0x17FC, 0);
    n = nv2a_pb_exec_last_batch(&prim);
    CHECK(n == 6 && prim == 5, "two DRAW_ARRAYS runs are ONE batch of 6 (%u)", n);
    CHECK(nv2a_drop_count(SHORT) == 0, "no short remnant from a split DRAW_ARRAYS either");

    /* CONTROL: a real two-index line batch. */
    short0 = nv2a_drop_count(SHORT);
    put(0x17FC, 2);                                          /* BEGIN LINES */
    put(0x1800, 0x00010000u);
    put(0x17FC, 0);
    CHECK(nv2a_drop_count(SHORT) == short0 + 1, "CONTROL: a genuine 2-index line batch IS counted as a short drop");

    /* CONTROL: immediate-mode positions. */
    put(0x17FC, 5);
    for (unsigned v = 0; v < 3; ++v)
        for (unsigned k = 0; k < 4; ++k) put(0x1A00 + 4 * k, 0x3F800000u);
    put(0x17FC, 0);
    CHECK(nv2a_drop_count(IMM) == 1, "an immediate-mode Begin/End is counted, not silent (%llu)", nv2a_drop_count(IMM));

    /* CONTROL: a Begin over an open batch. */
    put(0x17FC, 5);
    put(0x1800, 0x00010000u);
    put(0x17FC, 5);
    put(0x17FC, 0);
    CHECK(nv2a_drop_count(OPEN) == 1, "a Begin over an open batch is counted");

    printf("%s: %d failure%s\n", fails ? "FAIL" : "PASS", fails, fails == 1 ? "" : "s");
    return fails ? 1 : 0;
}
